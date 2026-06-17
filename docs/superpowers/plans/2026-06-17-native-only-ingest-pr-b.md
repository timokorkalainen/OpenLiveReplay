# Native-only ingest (PR B) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** OpenLiveReplay ingests only native SRT + native RTMP; remove `FfmpegIngestSession` + the native→ffmpeg fallback, make native SRT the default, and add a Windows Media Foundation AAC decoder (renaming the AAC decoder to `NativeAacDecoder`) so Windows keeps audio without ffmpeg.

**Architecture:** Three cohesive tasks: (1) rename `AudioToolboxAacDecoder`→`NativeAacDecoder` with platform TUs + a Windows MF implementation; (2) excise the ffmpeg ingest path + fallback machinery and make the selector native-only (`srt`→NativeSrt, `rtmp/rtmps`→NativeRtmp, else→Unsupported); (3) migrate the udp:// e2e tests to the native SRT bridge and delete the ffmpeg-SRT duplicate gates + docs. FFmpeg stays linked for the muxer/encoder.

**Tech Stack:** C++17, Qt6, FFmpeg (muxer/encoder only after this PR), Media Foundation (Windows AAC), AudioToolbox (Apple AAC), Qt Test, bash e2e (`srt-live-transmit`).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-06-17-native-only-ingest-design.md`.
- **Base branch:** `feat/ffmpeg-ingest-removal`, stacked on `feat/rtmp-parity`. Build dir `build/ingest`. **Local-only — NO push** (user hold).
- Format only changed C++ lines: after `git add`, `PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format`, re-`git add`. CI lint = clang-format 22.1.7; never whole-file format.
- FFmpeg remains a dependency (muxer `muxer.cpp`, MPEG-2 encoder in `streamworker.cpp`). Only the *ingest* use of ffmpeg is removed.
- Apple AAC + the macOS app/tests are verified locally (this is macOS). The Windows MF decoder is **compile-checked only** here (Windows CI build) + live-smoke-tested by the user.
- Verified code anchors (read 2026-06-17): `audiotoolboxaacdecoder.h` (the `NativeAacDecoder`-to-be API: `AacAdtsFrameInfo`; static `parseAdtsFrame`/`hasAdtsSync`/`hasLatmLoasSync`; `decodeAdtsFrame(frame,info,QByteArray* pcmS16Stereo,QString* error)`; `reset()`; pimpl `Impl*`). `audiotoolboxaacdecoder.mm` (Apple, `#ifdef __APPLE__`, 48k/2ch/S16 output, AAC-LC, the `kNoMoreInputData` EOS sentinel). `audiotoolboxaacdecoder_stub.cpp` (`#ifndef __APPLE__`, full parse + no-op decode). `streamworker.cpp:341-432` (backend select + construct + the `suppressNativeForCurrentUrl`/`nativeFallbackReason` fallback + `m_targetFps` only in the Ffmpeg ctor). `ingestsession.{h,cpp}` (`IngestBackendKind{Ffmpeg,NativeSrt,NativeRtmp}`, `selectIngestBackend`, `ingestBackendOptionsFromEnvironment` with the `OLR_NATIVE_SRT` gate, `augmentSrtUrl`, `nativeFallbackReason()` virtual, `IngestFailureKind`, `shouldStopNativeRtmpAfterFailure`). `tst_ingestbackendselector.cpp` (ffmpeg-default + fallback cases to drop; RTMP-anchor cases to keep). `tst_audiotoolboxaacdecoder.cpp` (4 parse cases). `tests/e2e/srt_lib.sh` (`srt_bridge`, `srt_caller_url`, `srt_require_tools`). `tests/e2e/CMakeLists.txt:119-227` (`e2e_srt_*` `srt`-label gates + the native-srt `OLR_NATIVE_SRT=1` env). `tests/CMakeLists.txt:64-71` (`OLR_FFMPEG_SRT_PREFIX`). `.github/workflows/ci.yml` (sanitizer matrix names `tst_audiotoolboxaacdecoder`).

---

### Task 1: Rename `AudioToolboxAacDecoder` → `NativeAacDecoder` + Windows MF AAC decoder

**Files:**
- Rename: `recorder_engine/ingest/audiotoolboxaacdecoder.h`→`nativeaacdecoder.h`; `audiotoolboxaacdecoder.mm`→`nativeaacdecoder_audiotoolbox.mm`; `audiotoolboxaacdecoder_stub.cpp`→`nativeaacdecoder_stub.cpp`; `tests/unit/tst_audiotoolboxaacdecoder.cpp`→`tests/unit/tst_nativeaacdecoder.cpp`.
- Create: `recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp`.
- Modify: `recorder_engine/ingest/nativesrtingestsession.{h,cpp}`, `recorder_engine/ingest/nativertmpingestsession.{h,cpp}` (include + class name), `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`, `.github/workflows/ci.yml`.

**Interfaces:**
- Produces: class `NativeAacDecoder` (same API as the old `AudioToolboxAacDecoder`): `static bool parseAdtsFrame(const QByteArray&, int, AacAdtsFrameInfo*)`, `static bool hasAdtsSync(...)`, `static bool hasLatmLoasSync(...)`, `bool decodeAdtsFrame(const QByteArray& frame, const AacAdtsFrameInfo& info, QByteArray* pcmS16Stereo, QString* error)`, `void reset()`. `struct AacAdtsFrameInfo` unchanged.

- [ ] **Step 1: Rename the header.** `git mv recorder_engine/ingest/audiotoolboxaacdecoder.h recorder_engine/ingest/nativeaacdecoder.h`. Edit it: include-guard `AUDIOTOOLBOXAACDECODER_H`→`NATIVEAACDECODER_H`; class `AudioToolboxAacDecoder`→`NativeAacDecoder` (ctor/dtor/copy-delete too). Add a one-line class comment: `// Native AAC (ADTS) decoder: AudioToolbox on Apple, Media Foundation on Windows, stub elsewhere.`

- [ ] **Step 2: Rename the Apple + stub TUs.** `git mv .../audiotoolboxaacdecoder.mm .../nativeaacdecoder_audiotoolbox.mm` and `git mv .../audiotoolboxaacdecoder_stub.cpp .../nativeaacdecoder_stub.cpp`. In BOTH: change `#include "audiotoolboxaacdecoder.h"`→`#include "nativeaacdecoder.h"` and every `AudioToolboxAacDecoder::`→`NativeAacDecoder::`. In `nativeaacdecoder_stub.cpp` narrow the guard `#ifndef __APPLE__`→`#if !defined(__APPLE__) && !defined(_WIN32)` (both the opening `#if` and update the closing `#endif // !__APPLE__` comment). Leave all decode/parse logic otherwise unchanged.

- [ ] **Step 3: Update the two callers.** In `nativesrtingestsession.{h,cpp}` and `nativertmpingestsession.{h,cpp}`: `#include "audiotoolboxaacdecoder.h"`→`#include "nativeaacdecoder.h"`; every `AudioToolboxAacDecoder`→`NativeAacDecoder` (member types `std::unique_ptr<AudioToolboxAacDecoder>`, the `AudioToolboxAacDecoder::parseAdtsFrame` static calls, `std::make_unique<AudioToolboxAacDecoder>()`). Grep both files to catch every hit.

- [ ] **Step 4: Rename the test.** `git mv tests/unit/tst_audiotoolboxaacdecoder.cpp tests/unit/tst_nativeaacdecoder.cpp`. Edit: `#include ".../audiotoolboxaacdecoder.h"`→`.../nativeaacdecoder.h`; class `TestAudioToolboxAacDecoder`→`TestNativeAacDecoder`; the `AudioToolboxAacDecoder::parseAdtsFrame`/`::hasAdtsSync`/`::hasLatmLoasSync` static calls→`NativeAacDecoder::`; `QTEST_GUILESS_MAIN(TestNativeAacDecoder)`; the moc include `#include "tst_audiotoolboxaacdecoder.moc"`→`#include "tst_nativeaacdecoder.moc"`. Do NOT change the 4 test cases' assertions.

- [ ] **Step 5: Update CMake + ci.yml for the renames.**
  - `CMakeLists.txt` (app recorder_engine source list) + `tests/CMakeLists.txt` (`olr_test_*` source list): replace the AAC source entries. Follow the `NativeVideoDecoder` pattern — select TUs by platform:
    ```cmake
    recorder_engine/ingest/nativeaacdecoder.h
    $<$<PLATFORM_ID:Darwin>:recorder_engine/ingest/nativeaacdecoder_audiotoolbox.mm>
    $<$<PLATFORM_ID:Windows>:recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp>
    $<$<NOT:$<OR:$<PLATFORM_ID:Darwin>,$<PLATFORM_ID:Windows>>>:recorder_engine/ingest/nativeaacdecoder_stub.cpp>
    ```
    READ how `nativevideodecoder_*` is wired in these files first and match that exact style (it may use `if(APPLE)/elseif(WIN32)/else()` lists rather than generator expressions — mirror whichever the video decoder uses).
  - `tests/unit/CMakeLists.txt`: rename the `olr_add_unit_test(tst_audiotoolboxaacdecoder ...)` registration to `tst_nativeaacdecoder` (same lib).
  - `.github/workflows/ci.yml`: in the sanitizer matrix `ctest_regex`, replace `tst_audiotoolboxaacdecoder`→`tst_nativeaacdecoder`.

- [ ] **Step 6: Create `recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp`** (`#ifdef _WIN32`). First READ `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp` IN FULL — reuse its COM/MF lifecycle conventions (MFStartup/MFShutdown refcount, `ComPtr`/release patterns, error-string formatting, the `Impl` pimpl shape). Implement the `NativeAacDecoder` API for Windows:
  - **Parse statics** (`parseAdtsFrame`/`hasAdtsSync`/`hasLatmLoasSync` + the `kAdtsSampleRates` table + `byteAt`): copy them verbatim from `nativeaacdecoder_stub.cpp` (they are platform-agnostic; this keeps the parse unit tests running on Windows without a shared-TU refactor, matching the existing `.mm`/stub duplication).
  - **`Impl`**: holds the AAC decoder MFT (`IMFTransform`), the configured input `AudioStreamType`/ASC, and a flag for whether the MFT is initialized. ctor/dtor manage COM (`MFStartup(MFVERSION)` / `MFShutdown()` refcounted like the video decoder), `reset()` drains/reconfigures the MFT.
  - **`decodeAdtsFrame(frame, info, pcmS16Stereo, error)`**: 
    1. Lazily create the AAC decoder MFT via `MFTEnumEx` for `{ MFMediaType_Audio, MFAudioFormat_AAC }` (transform category `MFT_CATEGORY_AUDIO_DECODER`), or `CoCreateInstance(CLSID_CMSAACDecMFT)`.
    2. Set the **input** media type: `MFAudioFormat_AAC`, sample rate `info.sampleRate`, channels `info.channelCount`, plus the `HEAACWAVEINFO`/`MF_MT_USER_DATA` payload describing AAC-LC (object type from `info.audioObjectType`; reject `audioObjectType != 2` with a clear error → return false, matching the Apple AAC-LC-only contract).
    3. Set the **output** media type: `MFAudioFormat_PCM`, 16-bit, then feed the frame (`ProcessInput` with an `IMFSample` wrapping `frame` — note MF AAC wants the raw AAC, so strip the ADTS header using `info.headerSize`) and pull PCM (`ProcessOutput`).
    4. **Resample/remix to 48 kHz S16 stereo** (the engine contract): if the decoded PCM isn't already 48k/2ch, convert (a `MFTransform` resampler `CLSID_CResamplerMediaObject`, or a manual linear resample + mono→stereo duplicate — keep it simple and correct). Append the result to `*pcmS16Stereo`.
    5. On any HRESULT failure set `*error` (use the video decoder's HRESULT→QString helper) and return false; success returns true.
  - Keep it AAC-LC / 48k-S16-stereo to exactly match `nativeaacdecoder_audiotoolbox.mm`'s output contract (the engine downstream assumes 48k stereo S16).
  - This file cannot be built/run on macOS; it is compile-checked by the Windows CI job (`build.yml`) and live-smoke-tested by the user. Write it carefully against the MF video decoder + the MS "AAC Decoder" MFT docs.

- [ ] **Step 7: Build + test on macOS (the Apple path + the rename).** `cmake -S . -B build/ingest >/dev/null && cmake --build build/ingest` (clean; the MF TU is excluded on macOS by the platform guard, so it is NOT compiled here). `( cd build/ingest && ctest -R tst_nativeaacdecoder --output-on-failure )` → 4 parse cases pass. `grep -rn "AudioToolboxAacDecoder\|audiotoolboxaacdecoder" recorder_engine tests` → only the `.mm`'s internal AudioToolbox API calls may remain conceptually, but the class name + filenames must be gone (the `.mm` still uses Apple's `AudioToolbox/AudioToolbox.h` framework include — that's correct and stays; only the OLR class/file names change).

- [ ] **Step 8: Format + commit.**
```bash
git add -A recorder_engine tests CMakeLists.txt .github/workflows/ci.yml
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add -A recorder_engine tests CMakeLists.txt .github/workflows/ci.yml
git commit -m "feat(ingest): rename AudioToolboxAacDecoder->NativeAacDecoder + Windows MF AAC decoder"
```

---

### Task 2: Native-only ingest excision (remove ffmpeg path + fallback; selector → native-only)

**Files:**
- Delete: `recorder_engine/ingest/ffmpegingestsession.{h,cpp}`, `recorder_engine/ingest/nativefallbackpolicy.{h,cpp}`, `tests/unit/tst_srt_options.cpp`.
- Modify: `recorder_engine/ingest/ingestsession.{h,cpp}`, `recorder_engine/ingest/nativesrtingestsession.{h,cpp}`, `recorder_engine/streamworker.cpp`, `recorder_engine/ingest/nativertmpingestsession.{h,cpp}` (if it overrides `nativeFallbackReason`), `tests/unit/tst_ingestbackendselector.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt`.

**Interfaces:**
- Produces: `enum class IngestBackendKind { NativeSrt, NativeRtmp, Unsupported }`; `selectIngestBackend(url, options)` → `srt`→NativeSrt, `rtmp/rtmps`→NativeRtmp, else→Unsupported; `ingestBackendOptionsFromEnvironment` with SRT native by default (no `OLR_NATIVE_SRT`).

- [ ] **Step 1: Rewrite the selector unit test (TDD).** READ `tests/unit/tst_ingestbackendselector.cpp` fully. Remove the `#include "...ffmpegingestsession.h"` and `#include "...nativefallbackpolicy.h"` lines. Remove these slots + their bodies: `defaultRoutesEverythingToFfmpeg`, `canConstructFfmpegSession`, `nativeFailureReasonStartsEmpty`, `nativeDecodeCapabilityErrorsRequestFallback`, `nativeDecodeTransientErrorsDoNotRequestFallback` (and the now-unused `EmptySession` class if nothing else uses it). Change `nativeSrtFlagRoutesOnlySrtToNative` / `nativeRtmpFlagRoutesOnlyRtmpToNative` so the non-native scheme expects `IngestBackendKind::Unsupported` (not `Ffmpeg`). Replace `environmentDefaultsRtmpAndRtmpsToNative`/`legacyRtmpFfmpegOverridesAreIgnored`/`nativeRtmpDisableEnvIsIgnored` with the new env contract, and add native-only routing cases. Concretely the new non-RTMP-anchor test set:
```cpp
void TestIngestBackendSelector::srtRoutesToNativeSrt() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::rtmpRoutesToNativeRtmp() {
    IngestBackendOptions opts;
    opts.preferNativeRtmp = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::NativeRtmp);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::NativeRtmp);
}
void TestIngestBackendSelector::unsupportedSchemesAreRejected() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    opts.preferNativeRtmp = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("udp://127.0.0.1:1234")), opts),
             IngestBackendKind::Unsupported);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("file:///tmp/x.ts")), opts),
             IngestBackendKind::Unsupported);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("http://example.test/x")), opts),
             IngestBackendKind::Unsupported);
}
void TestIngestBackendSelector::srtIsNativeByDefaultWithoutEnv() {
    qunsetenv("OLR_NATIVE_SRT"); // no longer consulted
    const IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("srt://127.0.0.1:9000")), true, false);
    QVERIFY(opts.preferNativeSrt);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
}
void TestIngestBackendSelector::rtmpIsNativeByDefault() {
    const IngestBackendOptions opts =
        ingestBackendOptionsFromEnvironment(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), false, true);
    QVERIFY(opts.preferNativeRtmp);
}
```
  Update the `private slots:` declarations to match (remove the deleted, add the new). KEEP `nativeFailureStopsNativeRetryWithoutFfmpegFallback`, `sharedAnchorMapsStreamTime`, and the entire `#if defined(OLR_NATIVE_RTMP_AVAILABLE)` RTMP-anchor block (from PR A) unchanged.

- [ ] **Step 2: Remove `tst_srt_options` + its registration.** `git rm tests/unit/tst_srt_options.cpp`; remove its `olr_add_unit_test(tst_srt_options ...)` line from `tests/unit/CMakeLists.txt`. (It tests the soon-deleted `augmentSrtUrl`.)

- [ ] **Step 3: Verify RED** — `cmake -S . -B build/ingest >/dev/null 2>&1 ; cmake --build build/ingest --target tst_ingestbackendselector` fails (the test references `IngestBackendKind::Unsupported` which doesn't exist yet, and no longer includes the ffmpeg headers).

- [ ] **Step 4: `ingestsession.h`** — `enum class IngestBackendKind { NativeSrt, NativeRtmp, Unsupported };` (drop `Ffmpeg`). Remove the `nativeFallbackReason()` virtual from `class IngestSession`. Remove the `QUrl augmentSrtUrl(const QUrl&);` declaration + its doc comment. Keep `IngestFailureKind`, `IngestOpenResult`, `IngestBackendOptions`, `shouldStopNativeRtmpAfterFailure`, `jitterWindowMs`, `kSrtLatencyMs`/`kSrtConnectTimeoutMs`, and everything else.

- [ ] **Step 5: `ingestsession.cpp`** — rewrite `selectIngestBackend`:
```cpp
IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    const QString scheme = url.scheme().toLower();
    if (options.preferNativeSrt && scheme == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }
    if (options.preferNativeRtmp &&
        (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps"))) {
        return IngestBackendKind::NativeRtmp;
    }
    return IngestBackendKind::Unsupported;
}
```
  Rewrite `ingestBackendOptionsFromEnvironment` (SRT native by default — drop the `OLR_NATIVE_SRT` gate):
```cpp
IngestBackendOptions ingestBackendOptionsFromEnvironment(const QUrl& url, bool nativeSrtAvailable,
                                                         bool nativeRtmpAvailable) {
    IngestBackendOptions options;
    const QString scheme = url.scheme().toLower();
    options.preferNativeSrt = nativeSrtAvailable && scheme == QStringLiteral("srt");
    options.preferNativeRtmp = nativeRtmpAvailable && (scheme == QStringLiteral("rtmp") ||
                                                       scheme == QStringLiteral("rtmps"));
    return options;
}
```
  Delete the `augmentSrtUrl(...)` definition. Keep `srtHealth`/`rtmpHealth`/`jitterWindowMs`/`shouldStopNativeRtmpAfterFailure`.

- [ ] **Step 6: Delete the ffmpeg + fallback sources.** `git rm recorder_engine/ingest/ffmpegingestsession.h recorder_engine/ingest/ffmpegingestsession.cpp recorder_engine/ingest/nativefallbackpolicy.h recorder_engine/ingest/nativefallbackpolicy.cpp`.

- [ ] **Step 7: `nativesrtingestsession.{h,cpp}`** — remove the fallback machinery. Drop `#include "nativefallbackpolicy.h"`; remove the `nativeFallbackReason()` override (declaration + definition) and the `m_nativeFallbackReason` member if present; where a decode-capability error previously set the fallback reason, replace with a plain `log(...)` of the error (the source simply fails/retries the connection — no ffmpeg). Grep the two files for `nativeFallbackReason`/`nativeDecodeErrorRequestsFallback` and remove all uses.

- [ ] **Step 8: `nativertmpingestsession.{h,cpp}`** — if it declares/defines a `nativeFallbackReason()` override or includes `nativefallbackpolicy.h`, remove them (RTMP uses `lastFailureKind()` + `shouldStopNativeRtmpAfterFailure`, which stay). Grep to confirm.

- [ ] **Step 9: `streamworker.cpp`** — rewrite the backend block (`:341-432`). Replace the construction switch's `else { FfmpegIngestSession }` and the post-run fallback with native-only + an Unsupported branch. Remove the `#include "ingest/ffmpegingestsession.h"`. Remove the `suppressNativeForCurrentUrl` local (its declaration earlier in the function too) and the `if (suppressNativeForCurrentUrl) backendOptions.preferNativeSrt = false;` line. The new construction + post-open handling:
```cpp
        const IngestBackendKind backendKind = selectIngestBackend(sourceUrl, backendOptions);
        const bool nativeRtmpAttempt = backendKind == IngestBackendKind::NativeRtmp;

        std::unique_ptr<IngestSession> session;
#if defined(OLR_NATIVE_SRT_AVAILABLE)
        if (backendKind == IngestBackendKind::NativeSrt) {
            session = std::make_unique<NativeSrtIngestSession>(m_sourceIndex, m_targetWidth,
                                                               m_targetHeight, &m_captureRunning);
        }
#endif
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
        if (backendKind == IngestBackendKind::NativeRtmp) {
            session = std::make_unique<NativeRtmpIngestSession>(m_sourceIndex, m_targetWidth,
                                                                m_targetHeight, &m_captureRunning);
        }
#endif
        if (!session) {
            // Unsupported scheme, or this build has no native backend for it. OLR ingests
            // only srt:// rtmp:// rtmps:// — fail the source cleanly (no ffmpeg fallback).
            qWarning() << "Source" << m_sourceIndex << "unsupported ingest scheme"
                       << sourceUrl.scheme()
                       << "- OpenLiveReplay ingests only srt://, rtmp://, rtmps://";
            setConnected(false);
            m_captureRunning = false;
            break;
        }
```
  Then keep the existing `if (!session->open(...))` block BUT remove its `nativeFallbackReason`-related handling (the open-fail path already only checks `nativeRtmpAttempt && shouldStopNativeRtmpAfterFailure` + backoff — that stays). After `session->run();`, DELETE the `nativeFallbackReason`/`suppressNativeForCurrentUrl` block (`:417-423`); keep the `setConnected(false)` + the `nativeRtmpAttempt && shouldStopNativeRtmpAfterFailure` stop check (`:425-432`). `m_targetFps` stays (the encoder uses it; it was only *also* passed to the deleted Ffmpeg ctor).

- [ ] **Step 10: CMake** — drop the deleted sources. In `CMakeLists.txt` (app) + `tests/CMakeLists.txt` (`olr_test_*`): remove the `ffmpegingestsession.{h,cpp}` and `nativefallbackpolicy.{h,cpp}` entries. (FFmpeg link flags stay — muxer/encoder still need them.)

- [ ] **Step 11: Build + test GREEN.** `cmake -S . -B build/ingest >/dev/null && cmake --build build/ingest` (whole tree clean; app no longer compiles ffmpegingestsession). `( cd build/ingest && ctest -L unit --output-on-failure )` — all unit pass incl. the rewritten `tst_ingestbackendselector` and `tst_nativeaacdecoder`, minus the removed `tst_srt_options`. `grep -rn "FfmpegIngestSession\|nativefallbackpolicy\|nativeFallbackReason\|augmentSrtUrl\|IngestBackendKind::Ffmpeg\|OLR_NATIVE_SRT" recorder_engine` → nothing (the `OLR_NATIVE_SRT` env gate is gone; `OLR_NATIVE_SRT_AVAILABLE` the build macro is different and STAYS).

- [ ] **Step 12: Format + commit.**
```bash
git add -A recorder_engine tests CMakeLists.txt
PATH="/opt/homebrew/opt/llvm/bin:$PATH" git clang-format
git add -A recorder_engine tests CMakeLists.txt
git commit -m "feat(ingest): native-only backend selection; remove FfmpegIngestSession + fallback"
```

---

### Task 3: Migrate udp:// e2e to the SRT bridge; delete ffmpeg-SRT gates; docs

**Files:**
- Modify: `tests/e2e/run_record_e2e.sh`, `tests/e2e/run_sync_e2e.sh`, `tests/e2e/run_playback_e2e.sh`, `tests/e2e/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/e2e/SRT_README.md` (+ any `run_srt_*.sh` comment referencing `OLR_FFMPEG_SRT_PREFIX`).

- [ ] **Step 1: READ** `tests/e2e/srt_lib.sh` (the `srt_bridge <udp_port> <srt_port>` helper, `srt_caller_url`, `srt_require_tools`) and the current `run_record_e2e.sh` (how it produces the UDP MPEG-TS source + invokes `record_harness --url udp://...`). Understand the existing bridge pattern from `run_srt_smoke.sh`.

- [ ] **Step 2: Migrate `run_record_e2e.sh` to the SRT bridge.** Source `srt_lib.sh`; keep the ffmpeg `testsrc2`+sine producer but emit to a UDP port, add `srt_bridge "$UDP_PORT" "$SRT_PORT"`, and point `record_harness --url` at the `srt://` caller URL (`srt_caller_url "$SRT_PORT"` / `srt://127.0.0.1:$SRT_PORT?transtype=live`). Native SRT is the default now — no env var. SKIP (exit 0) if `srt-live-transmit` is missing (call `srt_require_tools`). Preserve the existing ffprobe assertions (track count, A/V end-alignment, the `fps2997` avg_frame_rate check if present on this branch — note: this branch is off `origin/main` which does NOT have P1, so `run_record_e2e.sh` here is the pre-P1 version; keep its existing scenarios). Each port distinct per scenario.

- [ ] **Step 3: Migrate `run_sync_e2e.sh`** the same way: each scenario's UDP producer (flash/beep markers) → `srt_bridge` → harness on `srt://`. Reuse the markers + ffprobe/measurement asserts. Keep `drift_2997`'s 29.97 producer through the bridge. SKIP if `srt-live-transmit` missing.

- [ ] **Step 4: Migrate `run_playback_e2e.sh`** — its record phase feeds udp://; switch that phase to the SRT bridge (same pattern). Playback (decode of the recorded MKV) is unchanged.

- [ ] **Step 5: Delete the ffmpeg-SRT duplicate gates + build option.** In `tests/e2e/CMakeLists.txt`: remove the `srt`-label `add_test`s `e2e_srt_smoke/e2e_srt_4cam/e2e_srt_sync/e2e_srt_trim/e2e_srt_connect` and their `set_tests_properties`. Remove the `ENVIRONMENT "OLR_NATIVE_SRT=1"` from every `e2e_native_srt_*` `set_tests_properties` (the env var no longer exists; native is default). In `tests/CMakeLists.txt`: remove the `OLR_FFMPEG_SRT_PREFIX` cache option + the `OLR_FFMPEG_INCLUDE`/`OLR_FFMPEG_LIBDIR` branch it gates (lines ~64-71) — verify nothing else references those vars first. (The `e2e_native_srt_*` gates keep using `run_srt_*.sh`, which bridge UDP→SRT and connect natively; they do not need ffmpeg-with-srt.)

- [ ] **Step 6: Docs.** Update `tests/e2e/SRT_README.md`: OLR ingests only native srt/rtmp; SRT is native by default (remove the `OLR_NATIVE_SRT` / `OLR_FFMPEG_SRT_PREFIX` instructions); the udp:// e2e now bridge to native SRT; udp/file dropped with clean rejection + no fallback; the new `NativeAacDecoder` (AudioToolbox/MediaFoundation/stub). Remove `OLR_FFMPEG_SRT_PREFIX` mentions from any `run_srt_*.sh` header comments (`run_srt_ui_stats.sh`, `run_srt_loss.sh`).

- [ ] **Step 7: Reconfigure + run the migrated gates.** `cmake -S . -B build/ingest >/dev/null && cmake --build build/ingest`. Then (these need `srt-live-transmit` — `brew install srt`):
```bash
( cd build/ingest && ctest -L e2e --output-on-failure )      # migrated record/play over native SRT
( cd build/ingest && ctest -L sync-report --output-on-failure )  # migrated sync scenarios
( cd build/ingest && ctest -L native-rtmp --output-on-failure )  # unchanged
```
  Expected: the migrated record/play/sync gates pass over native SRT; native-rtmp unchanged. Confirm `ctest -N` no longer lists `e2e_srt_smoke`/etc. (the `srt`-label gates are gone). If a migrated gate fails because a marker/assert assumed UDP framing, fix the script (don't weaken the assertion).

- [ ] **Step 8: Commit.**
```bash
git add -A tests
git commit -m "test(ingest): migrate udp:// e2e to native SRT bridge; drop ffmpeg-SRT gates + docs"
```

---

## After all tasks
- `( cd build/ingest && ctest -L unit --output-on-failure )` — incl. `tst_nativeaacdecoder`, rewritten `tst_ingestbackendselector`; no `tst_srt_options`.
- `( cd build/ingest && ctest -L e2e )`, `-L sync-report`, `-L native-rtmp`, `-L native-apple-ingest` — all green on native sources; ffmpeg-SRT gates gone.
- `grep -rn "FfmpegIngestSession\|nativefallbackpolicy\|augmentSrtUrl\|AudioToolboxAacDecoder\|OLR_FFMPEG_SRT_PREFIX\|IngestBackendKind::Ffmpeg" recorder_engine tests CMakeLists.txt` → empty (the `OLR_NATIVE_SRT` *runtime* env gone; `OLR_NATIVE_SRT_AVAILABLE` build macro stays).
- Confirm FFmpeg still links (`muxer.cpp`/encoder) and the macOS app builds + records over native SRT/RTMP.
- Final code review over the branch (focus: the Unsupported-scheme path, no leftover fallback, SRT-default correctness, the AAC rename completeness, the Windows MF decoder against the MF video-decoder conventions).
- **Windows:** push triggers the Windows CI build (compile-check of the MF decoder) — but per the user hold, do NOT push; note it for when the hold lifts. The user will smoke-test live Windows AAC decode.
- Rebase onto latest `feat/rtmp-parity` (or `main` once PR A merges); open PR with base `feat/rtmp-parity`. **Do NOT push until the user re-grants permission.**

## Self-review
- **Spec coverage:** Component 1 (rename + MF AAC) → Task 1; Component 2 (excision + selector + SRT-default + unit tests) → Task 2; Component 3 (test migration + delete gates + build-config) → Task 3; Component 4 (docs) → Task 3 Step 6. All covered.
- **Types consistent:** `NativeAacDecoder`, `IngestBackendKind::{NativeSrt,NativeRtmp,Unsupported}`, `selectIngestBackend`, `ingestBackendOptionsFromEnvironment` used identically across tasks.
- **YAGNI:** no shared-parse-TU refactor (the MF TU copies the parse statics, matching the existing `.mm`/stub duplication — lower risk); no UI URL validation; no native file reader; no Windows CI test runner.
