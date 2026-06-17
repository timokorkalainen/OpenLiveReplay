# Frame-Sync Phase 2 — Native NDI Ingest — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add `NativeNdiIngestSession` — receive an `ndi://` source via the NDI SDK (reusing #54's runtime-`QLibrary` loading), convert NDI video/audio to the engine's `DecodedVideoFrame`/`DecodedAudioChunk`, and feed NDI's native timestamp + timecode into the timing core — making NDI both a new broadcast-quality input and the cleanest clock+TC source.

**Architecture:** Mirror #54's NDI **output** abstraction (`INdiSenderBackend` + `NdiOutputSink` + a `QLibrary`-loaded `NdiDynamicSenderBackend`) on the **receive** side: `INdiReceiverBackend` + `NativeNdiIngestSession : IngestSession` + a `QLibrary`-loaded `NdiDynamicReceiverBackend`. The session is dependency-injected with the backend (fake backend → full unit testing without the SDK). NDI delivers already-decoded frames, so there is **no** `NativeVideoDecoder`/`NativeAacDecoder` — the session converts NDI UYVY/BGRA/I420 → `AV_PIX_FMT_YUV420P` (swscale, already linked) and NDI float-planar audio → 48 kHz S16 stereo. NDI carries its own timestamp + timecode, so it plugs straight into the Phase-1 `SourceClock`/anchor (quality `Ndi`).

**Tech Stack:** C++17, Qt6 (`QLibrary`), FFmpeg (AVFrame + swscale), the NDI SDK (runtime-loaded), Qt Test.

## Global Constraints
- Spec: `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md` (Phase 2 + the NDI-ingest section). Pairs with Phase 1 (the timing core) but is largely independent — can land before or after.
- Worktree `/tmp/olr-bcast`. Build `build/bcast`. Format only changed C++ lines.
- **No new CMake `find_library`/include for NDI** — it is `QLibrary`-loaded at runtime exactly like #54 (`ndisink.cpp` `ensureLoaded()`/`resolveSymbols()`, runtime path via `OLR_NDI_RUNTIME_LIBRARY` / platform defaults). NDI is cross-platform, so the new sources go in the **unconditional** source list (near `ndisink.cpp`, root `CMakeLists.txt:120`) and `olr_test_engine` — NOT inside the `if(APPLE)/elseif(WIN32)` native blocks.
- Verified anchors (from the map): `INdiSenderBackend` `ndisink.h:8-16`; `NdiDynamicSenderBackend`/`ensureLoaded`/`resolveSymbols` `ndisink.cpp:80-251` (inline `NDIlib_*` typedefs `:26-68`, FLTp pack `:214-221`); `IngestSession`/`IngestCallbacks`/`DecodedVideoFrame`/`DecodedAudioChunk` `ingestsession.h:37-111`; `IngestBackendKind` `:16`, `selectIngestBackend`/`ingestBackendOptionsFromEnvironment` `ingestsession.cpp:5-67`; StreamWorker construction switch `streamworker.cpp:333-376` (ctor `(sourceIndex,w,h,&captureRunning)`); `NativeRtmpIngestSession::supportsUrl` `:212-216`; AVFrame build pattern `nativevideodecoder_videotoolbox.mm:129-191` (`av_frame_alloc`/`av_frame_get_buffer`/per-plane copy); `kAudioSampleRate=48000`; fake-backend test pattern `tst_ndisink.cpp:13-39`; selector tests `tst_ingestbackendselector.cpp:32-69`; the `friend class TestIngestBackendSelector` + `#if defined(QT_TESTLIB_LIB)` seam `nativertmpingestsession.h:22-24`.

---

### Task 1: Selector plumbing for `ndi://`

**Files:** Modify `recorder_engine/ingest/ingestsession.{h,cpp}`, `tests/unit/tst_ingestbackendselector.cpp`.

- [ ] **Step 1: Failing tests** in `tst_ingestbackendselector.cpp` (mirror `srtRoutesToNativeSrt`/`rtmpRoutesToNativeRtmp` at `:32-69`):
```cpp
void TestIngestBackendSelector::ndiRoutesToNativeNdi() {
    IngestBackendOptions opts; opts.preferNativeNdi = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("ndi://STUDIO%20(CAM1)")), opts),
             IngestBackendKind::NativeNdi);
}
void TestIngestBackendSelector::ndiIsNativeByDefaultWhenAvailable() {
    const IngestBackendOptions opts = ingestBackendOptionsFromEnvironment(
        QUrl(QStringLiteral("ndi://CAM1")), /*srt*/false, /*rtmp*/false, /*ndi*/true);
    QVERIFY(opts.preferNativeNdi);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("ndi://CAM1")), opts),
             IngestBackendKind::NativeNdi);
}
```
Add an `ndi://` case to `unsupportedSchemesAreRejected`'s **positive**-when-available expectation. Declare the slots. Verify RED (`NativeNdi`/`preferNativeNdi`/the 4-arg env fn undeclared).
- [ ] **Step 2: Extend the model.** `ingestsession.h`: `enum class IngestBackendKind { NativeSrt, NativeRtmp, NativeNdi, Unsupported };`; add `bool preferNativeNdi = false;` to `IngestBackendOptions`; change `ingestBackendOptionsFromEnvironment(const QUrl&, bool nativeSrtAvailable, bool nativeRtmpAvailable, bool nativeNdiAvailable)`. `ingestsession.cpp`: add to `selectIngestBackend` (before the `return Unsupported`): `if (options.preferNativeNdi && scheme == QStringLiteral("ndi")) return IngestBackendKind::NativeNdi;`; in `ingestBackendOptionsFromEnvironment` set `options.preferNativeNdi = nativeNdiAvailable && scheme == QStringLiteral("ndi");`. Update the existing 3-arg callers (StreamWorker — Task 6) to pass the new `nativeNdiAvailable`.
- [ ] **Step 3:** Build + run `tst_ingestbackendselector` → PASS. Commit `feat(ndi): selector routes ndi:// to NativeNdi`.

---

### Task 2: `INdiReceiverBackend` + a fake + `NativeNdiIngestSession` lifecycle

**Files:** Create `recorder_engine/ingest/nativendiingestsession.h`/`.cpp`, `tests/unit/tst_ndiingest.cpp`; modify CMake (root `CMakeLists.txt` unconditional sources, `tests/CMakeLists.txt` `olr_test_engine`, `tests/unit/CMakeLists.txt`).

**Interfaces — Produces:**
```cpp
// A decoded NDI frame handed to the session (already-decoded by the SDK).
struct NdiVideoFrame { int width, height, strideBytes; int fourCc; const uint8_t* data;
                       int64_t timestamp100ns; int64_t timecode100ns; };
struct NdiAudioFrame { int sampleRate, channels, samples, channelStrideBytes; const float* data;
                       int64_t timestamp100ns; int64_t timecode100ns; };
class INdiReceiverBackend {
public:
    virtual ~INdiReceiverBackend() = default;
    virtual bool isRuntimeAvailable() const = 0;
    virtual bool openReceiver(const QString& sourceName) = 0;
    virtual void closeReceiver() = 0;
    // Blocks up to timeoutMs; returns Video/Audio/None/Error; fills exactly one frame ptr.
    enum class Capture { None, Video, Audio, Error };
    virtual Capture capture(NdiVideoFrame* v, NdiAudioFrame* a, int timeoutMs) = 0;
    virtual void freeVideo(NdiVideoFrame* v) = 0;
    virtual void freeAudio(NdiAudioFrame* a) = 0;
};
class NativeNdiIngestSession final : public IngestSession {
public:
    NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning);                  // owns a real backend
    NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning, INdiReceiverBackend* backend); // DI
    static bool supportsUrl(const QUrl& url); // scheme=="ndi" && non-empty source name
    bool open(const QUrl&, const IngestCallbacks&) override;
    void run() override;
    void requestStop() override;
    IngestFailureKind lastFailureKind() const override;
};
```
Mirror the `friend class TestIngestBackendSelector` + `#if defined(QT_TESTLIB_LIB)` seam so the test can inject callbacks/backend.

- [ ] **Step 1: Failing test** `tst_ndiingest.cpp` with a `FakeNdiReceiverBackend` (model `tst_ndisink.cpp:13-39`) that returns a scripted queue from `capture(...)` (e.g. 2 video + 1 audio frame then `None`), an `isRuntimeAvailable()` toggle, and records open/close. Drive the session: set `m_callbacks` (recordingClockMs→a fixed clock; onVideoFrame/onAudioChunk→capture into locals), `open(QUrl("ndi://CAM1"), cb)` → true; `run()` (with `captureRunning` flipped false after a few iterations) → asserts the callbacks received the converted frames (a YUV420P `AVFrame*` at `outputW×outputH`; an S16-stereo `DecodedAudioChunk`); `supportsUrl(QUrl("ndi://CAM1"))==true`, `supportsUrl(QUrl("rtmp://x"))==false`. Register `olr_add_unit_test(tst_ndiingest olr_test_engine)`. Verify RED.
- [ ] **Step 2: Implement the session skeleton** (`open` stores callbacks + opens the backend on the parsed source name; `run` loops `capture()` → on Video convert+`onVideoFrame`, on Audio convert+`onAudioChunk`, on None continue, on Error set `lastFailureKind=TransientNetwork`+break; honor `shouldStop`/`captureRunning`; `setConnected(true)` after open, `(false)` on exit). Conversions are stubs in this task (return a black YUV420P frame + silence) so the lifecycle test passes; Tasks 3-4 fill the real conversion. `supportsUrl` per the interface. Build + run → lifecycle test PASS.
- [ ] **Step 3: CMake** — add `nativendiingestsession.{h,cpp}` to the root `CMakeLists.txt` **unconditional** source list (near `ndisink.cpp`) and to `tests/CMakeLists.txt` `olr_test_engine`. Build the app + tests clean. Commit `feat(ndi): NativeNdiIngestSession lifecycle + INdiReceiverBackend (fake-tested)`.

---

### Task 3: NDI video → `AV_PIX_FMT_YUV420P` conversion

**Files:** Create `recorder_engine/ingest/ndiframeconvert.h`/`.cpp` (pure helpers), `tests/unit/tst_ndiframeconvert.cpp`; use them in `nativendiingestsession.cpp`.

**Interfaces — Produces:** `AVFrame* ndiVideoToYuv420p(const NdiVideoFrame& in, int outW, int outH, SwsContext** cache)` — allocates a heap `AVFrame` (YUV420P, `outW×outH`, `av_frame_get_buffer`) and fills it from the NDI frame, scaling/colour-converting via `sws_getCachedContext`+`sws_scale` from the source FourCC (`UYVY422`→`AV_PIX_FMT_UYVY422`, `BGRA`→`AV_PIX_FMT_BGRA`, `I420`→`AV_PIX_FMT_YUV420P`). Ownership transfers to the caller (StreamWorker frees — `streamworker.cpp:285-322`).

- [ ] **Step 1: Failing test** — feed a synthetic UYVY frame (a known solid colour) at e.g. 64×48, convert to YUV420P at 32×24, and assert the output `AVFrame` dims/format + that the centre Y/U/V are within tolerance of the expected luma/chroma for that colour. Also an `I420`-at-target-size fast path (a plain per-plane copy, no swscale). Register `olr_add_unit_test(tst_ndiframeconvert olr_test_engine)`. RED.
- [ ] **Step 2: Implement** `ndiVideoToYuv420p` (model the alloc/copy in `nativevideodecoder_videotoolbox.mm:129-191`; swscale is already linked via `olr_test_engine`). Build + run → PASS. Use it in the session's video path (replace the Task-2 black stub).
- [ ] **Step 3:** Verify the lifecycle test still passes with real conversion (the fake backend now feeds a real UYVY buffer). Commit `feat(ndi): NDI video -> YUV420P conversion (swscale)`.

---

### Task 4: NDI audio (float-planar) → 48 kHz S16 stereo

**Files:** Extend `ndiframeconvert.{h,cpp}` + `tst_ndiframeconvert.cpp`; use in the session.

**Interfaces — Produces:** `QByteArray ndiAudioToS16Stereo(const NdiAudioFrame& in)` — float-planar (per-channel) → interleaved S16 stereo at 48 kHz: per sample `s16 = clamp(lrintf(planar[ch*stride + i] * 32768.0f), -32768, 32767)`; mono→duplicate; >2ch→take first two; resample to 48 kHz if `in.sampleRate != 48000` (a simple linear resample, mirroring the MF-AAC decoder's approach). This is the inverse of `ndisink.cpp:214-221`.

- [ ] **Step 1: Failing test** — a 2-channel float-planar frame (L=+0.5, R=−0.5, 48 kHz, 1024 samples) → S16 interleaved with `s16[0]≈16384`, `s16[1]≈-16384`; a mono frame → both channels equal; a 44.1 kHz frame → output sample count ≈ `samples*48000/44100`. RED → implement → PASS.
- [ ] **Step 2:** Use it in the session's audio path (replace the Task-2 silence stub); `chunk.startSample = sourcePtsMs * kAudioSampleRate / 1000` (the NDI timestamp/timecode → `sourcePtsMs` lands in Task 5). Commit `feat(ndi): NDI float-planar audio -> 48kHz S16 stereo`.

---

### Task 5: NDI timestamp + timecode → the timing core

NDI carries a monotonic 100 ns `timestamp` (sender clock) and a SMPTE `timecode`. Map both onto the recording timeline via the shared anchor (quality `Ndi`), so A/V stays locked and the timecode is available for Phase-3 alignment.

**Files:** Modify `nativendiingestsession.cpp`.

- [ ] **Step 1:** Convert NDI `timestamp100ns` → ms (`/10000`) and route through the same shared-anchor mapping the other sessions use (Phase 1's `AnchoredSourceClock{ClockQuality::Ndi}` if landed; otherwise inline the `sourcePtsMsForVideo/Audio` shared-anchor arithmetic from `nativertmpingestsession.cpp:1032-1077`). Video owns re-anchoring; audio follows. Set `sourcePtsMs` on both `DecodedVideoFrame` and `DecodedAudioChunk` from this mapping (not from arrival).
- [ ] **Step 2:** Stash the NDI `timecode100ns` per frame (a session member + on `IngestStats` as `int64_t timecode100ns` if Phase 1's stats fields exist) so Phase 3 (timecode alignment) can consume it. (Writing `tmcd` to the MKV is Phase 3.)
- [ ] **Step 3: Verify** the lifecycle test asserts the converted `sourcePtsMs` advances with the NDI timestamps (not the fixed clock) and that a co-timed audio frame maps to the same anchor as video. Commit `feat(ndi): map NDI timestamp/timecode through the shared anchor`.

---

### Task 6: Real `NdiDynamicReceiverBackend` (QLibrary) + StreamWorker wiring

**Files:** Add `NdiDynamicReceiverBackend` to `nativendiingestsession.cpp` (or a sibling `.cpp`); modify `recorder_engine/streamworker.cpp` (construction switch + the Unsupported allow-list + include).

- [ ] **Step 1: Implement `NdiDynamicReceiverBackend : INdiReceiverBackend`** mirroring `ndisink.cpp`'s `NdiDynamicSenderBackend` `ensureLoaded()`/`resolveSymbols()` exactly, swapping the resolved symbols to the **receive** side: `NDIlib_initialize`, `NDIlib_find_create_v2`, `NDIlib_find_get_current_sources`, `NDIlib_find_wait_for_sources`, `NDIlib_recv_create_v3`, `NDIlib_recv_capture_v3`, `NDIlib_recv_free_video_v2`, `NDIlib_recv_free_audio_v3`, `NDIlib_recv_destroy`, `NDIlib_find_destroy`. Declare the matching inline `NDIlib_recv_create_v3_t`/`NDIlib_find_create_t`/`NDIlib_*_frame_*` typedefs (mirror `ndisink.cpp:26-68`). `openReceiver(sourceName)` finds the source by name (substring match on `NDIlib_find_get_current_sources`), creates a receiver (prefer `NDIlib_recv_color_format_fastest`/UYVY+FLTp); `capture()` maps `NDIlib_frame_type_video/audio/none/error` to the `Capture` enum + fills the frame structs from the SDK frame fields (`timestamp`/`timecode` are 100 ns). **This TU is not unit-testable on a box without the NDI runtime** — it is exercised via the fake backend (Tasks 2-5) for logic and the real SDK at runtime. It compiles everywhere (QLibrary, no link dep).
- [ ] **Step 2: Wire StreamWorker** — `#include "ingest/nativendiingestsession.h"`; in `captureLoop` add `bool nativeNdiAvailable = NativeNdiIngestSession::supportsUrl(sourceUrl) && /*runtime*/ <a static INdiReceiverBackend availability probe>;` and pass it as the new 4th arg to `ingestBackendOptionsFromEnvironment`; add the construction clause `if (backendKind == IngestBackendKind::NativeNdi) session = std::make_unique<NativeNdiIngestSession>(m_sourceIndex, m_targetWidth, m_targetHeight, &m_captureRunning);` (no `#if` guard — NDI is cross-platform); extend the Unsupported `qWarning` allow-list to include `ndi://`.
- [ ] **Step 3:** Build the app + tests clean; `tst_ndiingest` + `tst_ingestbackendselector` green. (No real NDI source needed — the fake covers logic; the real path is a manual/integration check.) Commit `feat(ndi): runtime NDI receiver backend + StreamWorker wiring`.

---

### Task 7: e2e + docs

**Files:** Create `tests/e2e/run_ndi_smoke.sh` + register an opt-in `native-ndi` gate; update `docs/native-ingest-workstream-remaining.md`.

- [ ] **Step 1:** `run_ndi_smoke.sh` — if an NDI source is available (env `OLR_NDI_TEST_SOURCE`, e.g. an `ffmpeg -f lavfi … -f libndi_newtek` sender or the NDI Test Pattern app) and the NDI runtime is present, record via `record_harness --url "ndi://$OLR_NDI_TEST_SOURCE"` and assert the MKV has video+stereo audio + frames in band; else `SKIP 77`. Register `add_test(... LABELS "native-ndi" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)`. (The framesync rig's `ndi` transport cell from Phase 0 also exercises this once a sender exists.)
- [ ] **Step 2: Docs** — note NDI ingest is live (`ndi://` sources), the runtime-loaded SDK (same as the #54 sink), the frame conversions, and that NDI's native timestamp/timecode feed the timing core (the cleanest IP clock + the genlock bridge). Tick the NDI item. Commit `feat(ndi): ndi smoke gate + docs`.

---

## After all tasks
- `( cd build/bcast && ctest -L unit )` incl. `tst_ndiingest`, `tst_ndiframeconvert`, the `tst_ingestbackendselector` NDI cases.
- App + tests build clean on macOS (and Windows/Linux — NDI is `QLibrary`, no link dep); the real receiver path verified manually against an NDI sender.
- Final review: the QLibrary symbol mirror vs `ndisink.cpp`; the conversion correctness (colour + audio range); ownership of the heap `AVFrame`; NDI timestamp/timecode → anchor.

## Self-review
- **Spec coverage:** `NativeNdiIngestSession` + `INdiReceiverBackend` (mirroring #54) → Tasks 2,6; selector `ndi`→`NativeNdi` → Task 1; UYVY/BGRA/I420→YUV420P + FLTp→S16 conversions → Tasks 3,4; NDI timestamp+timecode into the timing core → Task 5; no CMake `find_library` (runtime QLibrary) → Tasks 2,6; fake-backend tests → all. Covered.
- **Types consistent:** `INdiReceiverBackend`/`NativeNdiIngestSession`/`NdiVideoFrame`/`NdiAudioFrame`/`ndiVideoToYuv420p`/`ndiAudioToS16Stereo`/`IngestBackendKind::NativeNdi`/`preferNativeNdi` used identically across tasks.
- **Independence:** Task 5 references Phase 1's `AnchoredSourceClock` but degrades to inline shared-anchor arithmetic if Phase 1 hasn't landed — so Phase 2 can ship standalone.
