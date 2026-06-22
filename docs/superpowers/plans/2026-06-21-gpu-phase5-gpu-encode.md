# gpu-encode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Feed the recorder's VideoToolbox (and Windows Media Foundation) H.264 encoder a **GPU-resident surface directly**, eliminating the per-frame CPU→`CVPixelBuffer` re-upload (`makeI420PixelBuffer`, `nativevideoencoder_videotoolbox.mm:27-52`) on the recorder `StreamWorker` jitter-pull pipeline. `VTCompressionSessionEncodeFrame` accepts an IOSurface-backed `CVPixelBuffer`, so a GPU-resident decoded frame's surface passes straight in with zero re-upload; on Windows the MF H.264 MFT accepts an `ID3D11Texture2D`-backed `IMFSample` (per the merged MF backend). The encode runs **async on a dedicated thread gated by a `gpu-sync` `GpuFence` BEFORE the encode call** so the encoder never reads a surface the decode/render queue is still writing and the master pull clock (`onMasterPulse`/`processEncoderTick`) never stalls. The encoded SPS carries correct **color VUI authored from `ColorMetadata`** (`playback/output/colormetadata.h`). The capstone proves a full **decode-GPU → encode-GPU stays-GPU round-trip** by extending `tst_h264_roundtrip` with a GPU-input variant: decode a keyframe to an IOSurface-backed `GpuSurface`, encode it straight from the surface, demux, decode back, and assert PSNR parity with the CPU-upload path within tolerance. This unblocks the recorder-engine redesign by removing the last CPU readback on the record path.

**Architecture:** Three layers stack on the merged Phase-0/1/2 abstraction and the `gpu-sync` keystone. (1) `NativeVideoEncoder` (`recorder_engine/codec/nativevideoencoder.h`) gains a GPU-surface encode entry point — `encodeSurface(GpuSurface*, ptsTicks, ColorMetadata, onPacket, error)` — implemented in `nativevideoencoder_videotoolbox.mm` by wrapping the surface's `IOSurfaceRef` in a `CVPixelBuffer` (`CVPixelBufferCreateWithIOSurface`) and passing it to `VTCompressionSessionEncodeFrame` with **no `makeI420PixelBuffer` copy**, and in `nativevideoencoder_mediafoundation.cpp` by binding the `ID3D11Texture2D` into an `IMFSample` via `MFCreateDXGISurfaceBuffer`. The session-create path authors the color VUI from `ColorMetadata` (`kVTCompressionPropertyKey_ColorPrimaries`/`_TransferFunction`/`_YCbCrMatrix` + full-range flag on Apple; `MF_MT_VIDEO_*` attributes on Windows). (2) A new `recorder_engine/codec/gpuencodepump.{h,cpp}` owns a dedicated `std::thread` that pulls `{GpuSurface, fenceValue, ptsTicks, ColorMetadata}` jobs off a bounded queue, waits the `GpuFence` to `fenceValue` (the producer's render/decode-done value), then calls `encodeSurface` and forwards packets through the same `PacketCallback` the synchronous path uses — so `processEncoderTick` enqueues a job instead of blocking on encode. (3) `StreamWorker` (`recorder_engine/streamworker.{h,cpp}`) branches its existing H.264 dispatch (`processEncoderTick`, `m_nativeEncoder->encode` at :252) on a GPU-input flag: when the held frame is a GPU-backed `FrameHandle`, it submits to the pump; otherwise the unchanged CPU `AVFrame` path runs. All of it is `#ifdef OLR_GPU_PIPELINE_BUILD` and behind `gpuPipelineEnabled()`; with the flag off, `StreamWorker` is byte-for-byte the merged CPU-upload recorder.

**Tech Stack:** C++17, Qt 6 (Core/Test), Apple VideoToolbox/CoreVideo/CoreMedia/IOSurface (`VTCompressionSessionEncodeFrame`, `CVPixelBufferCreateWithIOSurface`, VT color-property keys), Windows Media Foundation + D3D11 (`MFCreateDXGISurfaceBuffer`, `IMFSample`, `ID3D11Texture2D`), FFmpeg (`libav*` `AVFrame` at the CPU edge), CMake + Ninja. Consumes the merged Phase-0/1/2 + gpu-sync contracts verbatim: `FrameHandle`/`IFrameData`/`CpuPlanes`/`FrameMetadata`/`FramePayloadKey`/`FramePixelFormat`/`ColorMetadata` (`playback/output/framehandle.h`, `playback/output/colormetadata.h`), `GpuSurface`/`GpuSurfaceDesc` (`playback/gpu/gpusurface.h`), `GpuRhiContext` (`playback/gpu/gpurhicontext.h`), `GpuFrameData`/`makeGpuFrameHandle` (`playback/gpu/gpuframedata.h`), `GpuFence`/`makeD3D11GpuFence` (`playback/gpu/gpufence.h`, from gpu-sync), `DecodeDoneFence` (`playback/gpu/decodedonefence.h`), `gpuPipelineEnabled()` (`playback/gpu/gpupipelineconfig.h`), `NativeVideoEncoder`/`NativeVideoDecoder` (`recorder_engine/codec/nativevideoencoder.h`, `recorder_engine/ingest/nativevideodecoder.h`), `buildAvcCFromParameterSets`/`parseAvcc` (`recorder_engine/codec/avcc.h`).

## Global Constraints

- **Builds ON merged Phase-2 + gpu-sync, never replaces it.** Every interface named below either already exists in the tree (extend in place: `NativeVideoEncoder`, `StreamWorker::processEncoderTick`, `NativeVideoDecoder::decodeKeepSurface`) or is genuinely new (`encodeSurface`, `GpuEncodePump`). Use the **actual** signatures verified in the tree; do not invent variants. The gpu-sync `GpuFence` (`playback/gpu/gpufence.h`) — `signal()` / `wait(uint64_t,int)` / `completedValue()` / `create()` / `makeD3D11GpuFence(void*)` — must merge before this subproject (it is a `depends on: gpu-sync` edge in spec §6). If `gpufence.h` is absent, STOP and land gpu-sync first.
- **CPU path stays default + reference (spec D2/D5).** The GPU encode path is two-gated: the `OLR_GPU_PIPELINE` CMake option → `OLR_GPU_PIPELINE_BUILD` compile def, and the runtime `gpuPipelineEnabled()` env flag (off by default). With `OLR_GPU_PIPELINE_BUILD` undefined **or** `gpuPipelineEnabled()` false, every byte of this plan's behavior is inert: `StreamWorker` runs the existing `m_nativeEncoder->encode(m_latestFrame, ...)` CPU-upload path, `makeI420PixelBuffer` stays the encode source, and the recorder output is byte-identical to the merged recorder. The CPU-upload encode is the permanent correctness reference the GPU-input path is validated against (±PSNR-floor parity, Task 6).
- **Recorder, NOT a playback sink (spec §6 `gpu-encode` rescope).** This subproject lives entirely on the recorder `StreamWorker` jitter-pull pipeline (`onMasterPulse` → `processEncoderTick`), which is cleanly disjoint from the playback `OutputDispatcher`. It has its **own** clock (the master pulse). Do NOT touch `playback/output/outputdispatcher.*`, `async-readback`, or any playback sink. The only shared code is the `playback/gpu/` + `playback/output/` *types* (`GpuSurface`, `FrameHandle`, `ColorMetadata`, `GpuFence`), consumed read-only.
- **Fence BEFORE encode is non-negotiable (spec D4).** The encoder must never read a surface still being written by the decode/render queue. Every GPU-surface encode is gated by a `GpuFence::wait(fenceValue, timeoutMs)` that completes **before** `encodeSurface` runs, where `fenceValue` is the producer's decode-done/render value stamped on the job. The wait runs on the pump thread, never on the master-pulse tick thread, so the pull clock is never blocked. Each wait site carries a `// FENCE-BEFORE-ENCODE:` comment.
- **The pump must not stall the master pull clock.** `processEncoderTick` runs on the `StreamWorker` event-loop (tick) thread; it may only **enqueue** a job and return. The bounded queue drops the oldest job on overflow (the recorder is CFR — a dropped GPU job falls back to a repeat of the last encoded frame the same way the CPU path repeats `m_latestFrame`), never blocks the tick. A `gpuEncodeQueueDrops` counter records overflow.
- **Concurrency-critical — independent review required before merge (CLAUDE.md "Verification").** The pump thread, the bounded queue handoff, and the surface-lifetime hold (the job must retain the `FrameHandle` so its `GpuSurface` outlives the async encode) are TSan-relevant. Each task touching the pump threading carries a `**Review gate:**` note; the branch gets a fresh-agent concurrency review before the PR merges.
- **Zero-regression gate after every task.** With the flag off:
  ```sh
  cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
  ```
  must pass with identical assertion values, and `e2e_play` (`ctest -R e2e_play`) golden thresholds unchanged (the recorder e2e fixtures are unaffected). GPU behavioral tests `QSKIP` where no GPU/RHI/VideoToolbox backend exists (`offscreen`/CI), never hard-fail.
- **Build (run from the worktree root):** configure once with the GPU pipeline ON (this subproject only compiles its GPU paths under it):
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Also build once with `-DOLR_GPU_PIPELINE=OFF` (a fresh build dir) to confirm the off-path still compiles and the unit suite is byte-green. The recorder encoder lives in `olr_test_engine` (`tests/CMakeLists.txt:143` builds `recorder_engine/streamworker.cpp`; the VT encoder is in `olr_test_nativevideoencoder`, `tests/unit/CMakeLists.txt:160-183`). Register new unit tests via `olr_add_unit_test(<name> <libs...>)`. Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.
- **Format changed lines only** (CI Lint checks changed lines; recorder/engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
- **Public-repo professionalism.** Self-contained, professional code/comments/commits; document the present design, no internal notes or private history.

---

## Preconditions (read before Task 1)

- **gpu-sync merged.** `playback/gpu/gpufence.{h,gpufence_apple.mm,gpufence_stub.cpp,gpufence_win.cpp}` exist; `GpuFence::create()` / `GpuFence::signal()` / `GpuFence::wait(uint64_t,int)` / `GpuFence::completedValue()` / `makeD3D11GpuFence(void*)` are callable; `DecodeDoneFence` carries `signaledValue()` / `waitForValue(uint64_t,int)`; `GpuSurface` carries `retainUntilFenceRetired(uint64_t)` / `pendingFenceValue()`. Verify with `git merge-base --is-ancestor <gpu-sync-sha> origin/main` if unsure. If absent, land gpu-sync first.
- **Phase 2 merged.** `playback/gpu/{gpusurface.h,gpuframedata.h,vtkeepsurfaceimporter.h}`, `playback/output/{framehandle.h,colormetadata.h}`, and `recorder_engine/ingest/nativevideodecoder.h`'s `decodeKeepSurface(...)` + `lastDecodedWasIOSurfaceBacked()` all exist. The VT decode session already requests `kCVPixelBufferIOSurfacePropertiesKey` (`nativevideodecoder_videotoolbox.mm:356-359`), so `decodeKeepSurface` yields an IOSurface-backed `CVImageBufferRef`.
- **Today's encode source is a CPU re-upload.** `VideoToolboxEncoder::encode` (`nativevideoencoder_videotoolbox.mm:109-132`) calls `makeI420PixelBuffer(frame)` (:113) — an `I420` `CVPixelBufferCreate` + per-plane `memcpy` from the CPU `AVFrame` — then `VTCompressionSessionEncodeFrame`. The MF path (`nativevideoencoder_mediafoundation.cpp:561`, `buildInputSample`) `MFCreateMemoryBuffer` + a row-by-row NV12 pack. Both are the upload this subproject eliminates for GPU-resident frames.
- **The encode dispatch site is real.** `StreamWorker::processEncoderTick` (`streamworker.cpp:228-274`) holds the pulled frame in `m_latestFrame` (a CPU `AVFrame`) and, when `m_videoCodec == VideoCodecChoice::H264Hardware && m_nativeEncoder`, calls `m_nativeEncoder->encode(m_latestFrame, m_internalFrameCount, <callback writing each packet to m_muxer>, &encErr)`. The GPU path adds a parallel branch that submits a GPU surface to the pump; the muxer-write callback is reused verbatim. `setupEncoder` (`streamworker.cpp:555-577`) constructs `m_nativeEncoder` via `NativeVideoEncoder::create({w,h,fpsNum,fpsDen,bitrate}, &err)`.

---

## Task 1: `ColorMetadata` → VT/MF color-VUI mapping (pure function, no encoder yet)

**Precondition:** Phase 2 merged (`ColorMetadata` exists).

**Files:**
- Create: `recorder_engine/codec/colorvui.h`, `recorder_engine/codec/colorvui.cpp`
- Test: `tests/unit/tst_colorvui.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt` (add to `olr_test_engine`)

**Interfaces:**
- Consumes: `ColorMetadata`, `ColorMatrix`, `ColorRange`, `ColorPrimaries`, `ColorTransfer` (`playback/output/colormetadata.h`).
- Produces:
  ```cpp
  // recorder_engine/codec/colorvui.h — platform-neutral mapping of ColorMetadata
  // to the H.264 VUI colour-description triplet (ISO/IEC 23001-8 / H.273 code
  // points). The VT/MF encoders author these into the SPS so a downstream decoder
  // reconstructs the correct BT.601/709 + range instead of guessing from height.
  struct VuiColorCodePoints {
      int colourPrimaries = 1;          // H.273 ColourPrimaries (1 = BT.709)
      int transferCharacteristics = 1;  // H.273 TransferCharacteristics (1 = BT.709)
      int matrixCoefficients = 1;       // H.273 MatrixCoefficients (1 = BT.709)
      bool fullRange = false;           // video_full_range_flag
  };
  VuiColorCodePoints vuiColorCodePointsFor(const ColorMetadata& color);
  ```
  Mapping (H.273 code points): `ColorPrimaries::Bt709→1`, `Bt601→6` (SMPTE 170M / BT.601 525), `Bt2020→9`, `Unspecified→2`. `ColorTransfer::Bt709→1`, `Bt601→6`, `Bt2020→14`, `Unspecified→2`. `ColorMatrix::Bt709→1`, `Bt601→6`, `Bt2020→9`. `ColorRange::Full→true`, `Video→false`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_colorvui.cpp`:

```cpp
// vuiColorCodePointsFor maps ColorMetadata to the H.264 VUI colour-description
// triplet (H.273 code points) the VT/MF encoders author into the SPS, so a
// decoder reconstructs the exact colorimetry instead of the height>576 guess.
#include <QtTest>

#include "playback/output/colormetadata.h"
#include "recorder_engine/codec/colorvui.h"

class TestColorVui : public QObject {
    Q_OBJECT
private slots:
    void bt709VideoRangeIsOneOneOne();
    void bt601VideoRangeIsSixSixSix();
    void bt2020FullRange();
    void unspecifiedPrimariesAndTransferAreTwo();
};

void TestColorVui::bt709VideoRangeIsOneOneOne() {
    ColorMetadata c;  // defaults: BT.709 matrix/primaries/transfer, video range
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 1);
    QCOMPARE(v.transferCharacteristics, 1);
    QCOMPARE(v.matrixCoefficients, 1);
    QCOMPARE(v.fullRange, false);
}

void TestColorVui::bt601VideoRangeIsSixSixSix() {
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt601;
    c.primaries = ColorPrimaries::Bt601;
    c.transfer = ColorTransfer::Bt601;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 6);
    QCOMPARE(v.transferCharacteristics, 6);
    QCOMPARE(v.matrixCoefficients, 6);
    QCOMPARE(v.fullRange, false);
}

void TestColorVui::bt2020FullRange() {
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt2020;
    c.primaries = ColorPrimaries::Bt2020;
    c.transfer = ColorTransfer::Bt2020;
    c.range = ColorRange::Full;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 9);
    QCOMPARE(v.transferCharacteristics, 14);
    QCOMPARE(v.matrixCoefficients, 9);
    QCOMPARE(v.fullRange, true);
}

void TestColorVui::unspecifiedPrimariesAndTransferAreTwo() {
    ColorMetadata c;
    c.primaries = ColorPrimaries::Unspecified;
    c.transfer = ColorTransfer::Unspecified;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 2);
    QCOMPARE(v.transferCharacteristics, 2);
}

QTEST_GUILESS_MAIN(TestColorVui)
#include "tst_colorvui.moc"
```

Register in `tests/unit/CMakeLists.txt` (alongside the other engine tests, e.g. after `tst_h264_roundtrip`):

```cmake
olr_add_unit_test(tst_colorvui olr_test_engine)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_colorvui
```
Expected: FAIL to compile — `recorder_engine/codec/colorvui.h` not found.

- [ ] **Step 3: Write the implementation**

Create `recorder_engine/codec/colorvui.h` with the interface above. Create `recorder_engine/codec/colorvui.cpp`:

```cpp
#include "recorder_engine/codec/colorvui.h"

VuiColorCodePoints vuiColorCodePointsFor(const ColorMetadata& color) {
    VuiColorCodePoints v;
    switch (color.primaries) {
    case ColorPrimaries::Bt709: v.colourPrimaries = 1; break;
    case ColorPrimaries::Bt601: v.colourPrimaries = 6; break;
    case ColorPrimaries::Bt2020: v.colourPrimaries = 9; break;
    case ColorPrimaries::Unspecified: v.colourPrimaries = 2; break;
    }
    switch (color.transfer) {
    case ColorTransfer::Bt709: v.transferCharacteristics = 1; break;
    case ColorTransfer::Bt601: v.transferCharacteristics = 6; break;
    case ColorTransfer::Bt2020: v.transferCharacteristics = 14; break;
    case ColorTransfer::Unspecified: v.transferCharacteristics = 2; break;
    }
    switch (color.matrix) {
    case ColorMatrix::Bt709: v.matrixCoefficients = 1; break;
    case ColorMatrix::Bt601: v.matrixCoefficients = 6; break;
    case ColorMatrix::Bt2020: v.matrixCoefficients = 9; break;
    }
    v.fullRange = color.range == ColorRange::Full;
    return v;
}
```

Add `recorder_engine/codec/colorvui.cpp` to the `olr_test_engine` source list in `tests/CMakeLists.txt` (next to `recorder_engine/codec/avcc.cpp` at :143-145) and to the production `OpenLiveReplay` target's recorder sources in `CMakeLists.txt` (mirror where `recorder_engine/codec/avcc.cpp` is listed). `colorvui.{h,cpp}` are platform-neutral and compile on every platform unconditionally (no GPU gate — the VUI mapping is needed by both CPU and GPU encode paths in later tasks).

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_colorvui && ctest --test-dir build/c -R tst_colorvui --output-on-failure
```
Expected: PASS (4 tests).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/codec/colorvui.h recorder_engine/codec/colorvui.cpp \
        tests/unit/tst_colorvui.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-encode): ColorMetadata -> H.264 VUI colour code-point mapping"
```

---

## Task 2: Author the color VUI into the encoder session from `ColorMetadata`

**Precondition:** Task 1. A hardware H.264 encoder available on the test host (else `QSKIP`).

**Files:**
- Modify: `recorder_engine/codec/nativevideoencoder.h` (extend `Config` with optional color), `recorder_engine/codec/nativevideoencoder_videotoolbox.mm` (VT color properties), `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp` (MF color attributes), `recorder_engine/codec/nativevideoencoder_stub.cpp` (accept the new field, no-op)
- Test: `tests/unit/tst_h264_roundtrip.cpp` (extend — assert the demuxed SPS carries the requested code points)

**Interfaces:**
- Consumes: `VuiColorCodePoints`/`vuiColorCodePointsFor` (Task 1), `ColorMetadata`.
- Produces (extend the existing `Config`, ADD a field, default preserves today's output):
  ```cpp
  // recorder_engine/codec/nativevideoencoder.h, struct Config:
  struct Config {
      int width = 0;
      int height = 0;
      int fpsNum = 30;
      int fpsDen = 1;
      int bitrate = 30'000'000;
      ColorMetadata color;  // NEW: authored into the SPS VUI (default BT.709/video)
  };
  ```
  Add `#include "playback/output/colormetadata.h"` to `nativevideoencoder.h`.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_h264_roundtrip.cpp` a new slot that encodes a frame with `Config.color` set to BT.601 + full-range and asserts the demuxed SPS VUI reflects it. Parse the SPS colour-description with a small inline helper (the SPS is in the avcC extradata via `parseAvcc`). Declare:

```cpp
    void encodedSpsCarriesRequestedColorVui();
```

```cpp
void TestH264RoundTrip::encodedSpsCarriesRequestedColorVui() {
#ifdef _WIN32
    if (!qEnvironmentVariableIsSet("OLR_RUN_UNSTABLE_MF_H264_TESTS"))
        QSKIP("Windows Media Foundation H.264 round-trip is opt-in on this machine");
#endif
    QString err;
    NativeVideoEncoder::Config cfg{320, 240, 30, 1, 4'000'000};
    cfg.color.matrix = ColorMatrix::Bt601;
    cfg.color.primaries = ColorPrimaries::Bt601;
    cfg.color.transfer = ColorTransfer::Bt601;
    cfg.color.range = ColorRange::Full;
    auto enc = NativeVideoEncoder::create(cfg, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    AVFrame* f = av_frame_alloc();
    auto freeF = qScopeGuard([&] { av_frame_free(&f); });
    f->format = AV_PIX_FMT_YUV420P; f->width = 320; f->height = 240;
    QVERIFY(av_frame_get_buffer(f, 32) >= 0);
    memset(f->data[0], 128, f->linesize[0] * 240);
    memset(f->data[1], 128, f->linesize[1] * 120);
    memset(f->data[2], 128, f->linesize[2] * 120);

    bool got = false;
    QVERIFY(enc->encode(f, 0, [&](const QByteArray& d, int64_t, bool) { if (!d.isEmpty()) got = true; }, &err));
    if (!got) QSKIP("encoder produced no priming packet");
    const QByteArray avcc = enc->avccExtradata();
    if (avcc.isEmpty()) QSKIP("encoder exposed no avcC");

    QList<QByteArray> sps, pps;
    QVERIFY(parseAvcc(avcc, &sps, &pps));
    QVERIFY(!sps.isEmpty());
    // Parse the SPS VUI colour-description. The encoder MUST have signalled
    // video_full_range_flag + colour_description (primaries=6, transfer=6,
    // matrix=6 for BT.601). spsColourDescription() is the test-local parser below.
    VuiColorCodePoints got601;
    QVERIFY2(spsColourDescription(sps.first(), &got601),
             "encoded SPS carries no colour_description_present VUI");
    QCOMPARE(got601.colourPrimaries, 6);
    QCOMPARE(got601.transferCharacteristics, 6);
    QCOMPARE(got601.matrixCoefficients, 6);
    QCOMPARE(got601.fullRange, true);
}
```

Add a test-local SPS VUI parser (`spsColourDescription`) above the class — a minimal Exp-Golomb SPS walker that reads `video_full_range_flag` and the `colour_description_present_flag` triplet (it skips to the VUI by parsing the fixed SPS fields). Include `recorder_engine/codec/colorvui.h`, `recorder_engine/codec/avcc.h`, and `playback/output/colormetadata.h`.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_h264_roundtrip
```
Expected: FAIL to compile — `Config` has no `color` member.

- [ ] **Step 3: Write the implementation**

In `nativevideoencoder.h`, add `#include "playback/output/colormetadata.h"` and the `ColorMetadata color;` field to `Config`.

In `nativevideoencoder_videotoolbox.mm`, `NativeVideoEncoder::create`, after the existing `VTSessionSetProperty` calls (:172-181) and before `VTCompressionSessionPrepareToEncodeFrames`, author the color VUI from `cfg.color`:

```objcpp
    const VuiColorCodePoints vui = vuiColorCodePointsFor(cfg.color);
    // Map the H.273 code points to VideoToolbox color attachments so the SPS VUI
    // is authored correctly (replacing any height-inferred guess downstream).
    CFStringRef primaries = vui.colourPrimaries == 9 ? kCVImageBufferColorPrimaries_ITU_R_2020
                          : vui.colourPrimaries == 6 ? kCVImageBufferColorPrimaries_SMPTE_C
                          : kCVImageBufferColorPrimaries_ITU_R_709_2;
    CFStringRef transfer = vui.transferCharacteristics == 14 ? kCVImageBufferTransferFunction_ITU_R_2020
                         : vui.transferCharacteristics == 6 ? kCVImageBufferTransferFunction_ITU_R_709_2
                         : kCVImageBufferTransferFunction_ITU_R_709_2;
    CFStringRef matrix = vui.matrixCoefficients == 9 ? kCVImageBufferYCbCrMatrix_ITU_R_2020
                       : vui.matrixCoefficients == 6 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4
                       : kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ColorPrimaries, primaries);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_TransferFunction, transfer);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_YCbCrMatrix, matrix);
    // VideoToolbox writes video_full_range_flag into the SPS from the source
    // pixel-buffer's range; for a video-range NV12 source this is 0. Full-range
    // is authored at the surface level (Task 3 wraps the IOSurface with the
    // matching CVPixelBuffer color attachments). Record the request for that step.
    enc->requestedFullRange = vui.fullRange;
```
Add a `bool requestedFullRange = false;` member to `VideoToolboxEncoder` and `#include "recorder_engine/codec/colorvui.h"`.

In `nativevideoencoder_mediafoundation.cpp`, `configureInputType` (after the NV12 subtype is set, ~:398), set the MF color attributes from `vuiColorCodePointsFor(m_config.color)` — `MF_MT_VIDEO_PRIMARIES`, `MF_MT_TRANSFER_FUNCTION`, `MF_MT_YUV_MATRIX`, and `MF_MT_VIDEO_NOMINAL_RANGE` (`MFNominalRange_0_255` for full, `MFNominalRange_16_235` for video) — mapping the H.273 code points to the `MFVideoPrimaries`/`MFVideoTransferFunction`/`MFVideoTransferMatrix` enums. Include `recorder_engine/codec/colorvui.h`.

In `nativevideoencoder_stub.cpp`, the `Config` field needs no behavior — the stub already ignores config; confirm it still compiles with the new field (it will, since it includes the header).

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_h264_roundtrip && ctest --test-dir build/c -R tst_h264_roundtrip --output-on-failure
```
Expected: PASS on a VideoToolbox host (the existing `encodeMuxDemuxYieldsIntraH264` plus `encodedSpsCarriesRequestedColorVui`). The default `Config.color` (BT.709/video) keeps the existing test's SPS unchanged.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/codec/nativevideoencoder.h recorder_engine/codec/nativevideoencoder_videotoolbox.mm \
        recorder_engine/codec/nativevideoencoder_mediafoundation.cpp \
        recorder_engine/codec/nativevideoencoder_stub.cpp tests/unit/tst_h264_roundtrip.cpp
git commit -m "feat(gpu-encode): author SPS color VUI from ColorMetadata (VT + MF)"
```

---

## Task 3: `encodeSurface` — encode a GPU surface directly, no CPU re-upload

**Precondition:** Tasks 1-2; gpu-sync merged (`GpuSurface`). A VideoToolbox H.264 encoder + an IOSurface-backed surface available on the host (else `QSKIP`).

**Files:**
- Modify: `recorder_engine/codec/nativevideoencoder.h` (declare `encodeSurface`), `recorder_engine/codec/nativevideoencoder_videotoolbox.mm` (IOSurface→`CVPixelBuffer`→`VTCompressionSessionEncodeFrame`, no copy), `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp` (`ID3D11Texture2D`→`MFCreateDXGISurfaceBuffer`→`IMFSample`), `recorder_engine/codec/nativevideoencoder_stub.cpp` (return false)
- Test: `tests/unit/tst_gpu_encode_surface.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuSurface` (`playback/gpu/gpusurface.h`) — `nativeHandle()` returns an `IOSurfaceRef` on Apple / `ID3D11Texture2D*` on Windows; `desc()`. `ColorMetadata`.
- Produces (ADD to `NativeVideoEncoder`, pure-virtual; the surface-direct sibling of `encode`):
  ```cpp
  // recorder_engine/codec/nativevideoencoder.h, class NativeVideoEncoder:
  // Encode one GPU-resident surface (all-intra -> one keyframe packet),
  // synchronously draining output to onPacket. The surface's native backing
  // (IOSurface on Apple / ID3D11Texture2D on Windows) is wrapped WITHOUT any
  // CPU re-upload (no makeI420PixelBuffer / MFCreateMemoryBuffer copy). `color`
  // is informational here (the SPS VUI is already authored at session create,
  // Task 2); it sets the wrapped pixel buffer's range attachment so the encoder
  // signals video_full_range_flag correctly. The caller MUST have fenced the
  // surface (gpu-sync GpuFence) before calling. ptsTicks is opaque (echoed).
  virtual bool encodeSurface(GpuSurface* surface, int64_t ptsTicks, const ColorMetadata& color,
                             const PacketCallback& onPacket, QString* error) = 0;
  ```
  Add a forward declaration `class GpuSurface;` to `nativevideoencoder.h` (the header already forward-declares `struct AVFrame;`).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpu_encode_surface.cpp` — manufacture an IOSurface-backed NV12 `GpuSurface` (reuse the Phase-2 test helper `makeAppleNv12Surface(w,h)` from `tst_gpusurface.cpp`'s pattern, or fill an IOSurface directly), then `encodeSurface` it and assert a keyframe packet comes out:

```cpp
// encodeSurface feeds a GPU-resident surface straight to the hardware H.264
// encoder with NO CPU re-upload (no makeI420PixelBuffer). On a VideoToolbox host
// it produces an all-intra keyframe packet; off-GPU it QSKIPs.
#include <QtTest>

#include "playback/gpu/gpusurface.h"
#include "playback/output/colormetadata.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#ifdef __APPLE__
std::shared_ptr<GpuSurface> makeAppleNv12Surface(int w, int h);  // Phase-2 test helper
#endif

class TestGpuEncodeSurface : public QObject {
    Q_OBJECT
private slots:
    void encodesSurfaceToKeyframeWithoutCpuUpload();
};

void TestGpuEncodeSurface::encodesSurfaceToKeyframeWithoutCpuUpload() {
#ifndef __APPLE__
    QSKIP("GPU-surface encode test currently exercises the VideoToolbox host path");
#else
    auto surface = makeAppleNv12Surface(320, 240);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");
    QString err;
    NativeVideoEncoder::Config cfg{320, 240, 30, 1, 4'000'000};
    auto enc = NativeVideoEncoder::create(cfg, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    bool gotKeyframe = false;
    bool ok = enc->encodeSurface(
        surface.get(), 0, ColorMetadata{},
        [&](const QByteArray& d, int64_t, bool key) { if (!d.isEmpty() && key) gotKeyframe = true; },
        &err);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY(gotKeyframe);
    QVERIFY(!enc->avccExtradata().isEmpty());
#endif
}

QTEST_GUILESS_MAIN(TestGpuEncodeSurface)
#include "tst_gpu_encode_surface.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:

```cmake
    olr_add_unit_test(tst_gpu_encode_surface olr_test_engine olr_test_playback)
    target_link_libraries(tst_gpu_encode_surface PRIVATE olr_test_nativevideoencoder)
```
(`olr_test_playback` carries the Apple `GpuSurface`/`makeAppleNv12Surface` test helper; if the helper is file-local to `tst_gpusurface.cpp`, lift it into a tiny shared `tests/unit/gpusurfacefixture.{h,mm}` and link that instead — mirror `tests/unit/framepsnr.h`.)

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpu_encode_surface
```
Expected: FAIL to compile — `encodeSurface` is not a member of `NativeVideoEncoder`.

- [ ] **Step 3: Write the implementation**

In `nativevideoencoder.h`, add the `class GpuSurface;` forward decl and the `encodeSurface` pure-virtual.

In `nativevideoencoder_videotoolbox.mm`, implement `VideoToolboxEncoder::encodeSurface`. Wrap the surface's `IOSurfaceRef` in a `CVPixelBuffer` with `CVPixelBufferCreateWithIOSurface` (NO `makeI420PixelBuffer` copy), attach the range from `color`, then run the SAME `VTCompressionSessionEncodeFrame` body as `encode` (force-keyframe props, `CompleteFrames`, drain `sink`):

```objcpp
    bool encodeSurface(GpuSurface* surface, int64_t ptsTicks, const ColorMetadata& color,
                       const PacketCallback& onPacket, QString* error) override {
        if (!surface || !surface->isValid()) {
            if (error) *error = QStringLiteral("encodeSurface: null/invalid surface");
            return false;
        }
        auto* ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
        if (!ioSurface) {
            if (error) *error = QStringLiteral("encodeSurface: surface is not IOSurface-backed");
            return false;
        }
        // Attach the colorimetry so VideoToolbox signals video_full_range_flag /
        // matrix consistently with the session VUI (Task 2). No pixel copy.
        const void* aKeys[] = {kCVImageBufferYCbCrMatrixKey, kCVImageBufferColorPrimariesKey,
                               kCVImageBufferTransferFunctionKey};
        const VuiColorCodePoints vui = vuiColorCodePointsFor(color);
        const void* aVals[] = {
            vui.matrixCoefficients == 6 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4
                                        : kCVImageBufferYCbCrMatrix_ITU_R_709_2,
            vui.colourPrimaries == 6 ? kCVImageBufferColorPrimaries_SMPTE_C
                                     : kCVImageBufferColorPrimaries_ITU_R_709_2,
            kCVImageBufferTransferFunction_ITU_R_709_2};
        CFDictionaryRef attach = CFDictionaryCreate(kCFAllocatorDefault, aKeys, aVals, 3,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferRef pb = nullptr;
        const CVReturn r =
            CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, attach, &pb);
        if (attach) CFRelease(attach);
        if (r != kCVReturnSuccess || !pb) {
            if (error) *error = QStringLiteral("CVPixelBufferCreateWithIOSurface failed (%1)").arg(r);
            return false;
        }
        std::vector<EncodedPacket> packets;
        sink = &packets;
        const CMTime pts = CMTimeMake(ptsTicks, 90000);
        const void* fk[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* fv[] = {kCFBooleanTrue};
        CFDictionaryRef frameProps = CFDictionaryCreate(kCFAllocatorDefault, fk, fv, 1,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        const OSStatus st = VTCompressionSessionEncodeFrame(
            session, pb, pts, kCMTimeInvalid, frameProps, reinterpret_cast<void*>(ptsTicks), nullptr);
        if (frameProps) CFRelease(frameProps);
        CVPixelBufferRelease(pb);
        if (st != noErr) {
            if (error) *error = QStringLiteral("VTCompressionSessionEncodeFrame(surface) failed (%1)").arg(st);
            sink = nullptr;
            return false;
        }
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        sink = nullptr;
        for (auto& p : packets) onPacket(p.data, p.ptsTicks, p.keyframe);
        return true;
    }
```
Add `#include <IOSurface/IOSurface.h>` and `#include "playback/gpu/gpusurface.h"` to the `.mm`.

In `nativevideoencoder_mediafoundation.cpp`, implement `encodeSurface` by wrapping the `ID3D11Texture2D*` (`surface->nativeHandle()`) in a DXGI-surface media buffer via `MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer)`, `MFCreateSample` + `AddBuffer`, then the same submit/drain as `encode` (reuse `processInputSample`/`drainOutput`). No `MFCreateMemoryBuffer` copy.

In `nativevideoencoder_stub.cpp`, implement `encodeSurface` returning `false` with `*error = "GPU-surface encode unavailable on this platform"`.

**Review gate:** `encodeSurface` must not outlive the surface — but here it is synchronous (it completes frames before returning), so the caller's surface lifetime is trivially safe. The async hold is added in Task 4 (the pump). Flag the `.mm`/`.cpp` for review only insofar as the IOSurface/D3D-texture wrap must not retain the surface past return.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpu_encode_surface && ctest --test-dir build/c -R tst_gpu_encode_surface --output-on-failure
```
Expected: PASS on a VideoToolbox host; `QSKIP` elsewhere.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/codec/nativevideoencoder.h recorder_engine/codec/nativevideoencoder_videotoolbox.mm \
        recorder_engine/codec/nativevideoencoder_mediafoundation.cpp recorder_engine/codec/nativevideoencoder_stub.cpp \
        tests/unit/tst_gpu_encode_surface.cpp tests/unit/CMakeLists.txt
git commit -m "feat(gpu-encode): encodeSurface feeds IOSurface/D3D11 texture to the encoder (zero re-upload)"
```

---

## Task 4: `GpuEncodePump` — dedicated async encode thread, fence-gated before encode

**Precondition:** Task 3; gpu-sync merged (`GpuFence`). **This is the threading task; the master pull clock must not stall.**

**Files:**
- Create: `recorder_engine/codec/gpuencodepump.h`, `recorder_engine/codec/gpuencodepump.cpp`
- Test: `tests/unit/tst_gpuencodepump.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt` (add to `olr_test_engine`), `CMakeLists.txt`

**Interfaces:**
- Consumes: `NativeVideoEncoder::encodeSurface` (Task 3), `GpuFence` (`playback/gpu/gpufence.h`), `FrameHandle`/`GpuSurface` (`playback/output/framehandle.h`, `playback/gpu/gpusurface.h`), `ColorMetadata`.
- Produces:
  ```cpp
  // recorder_engine/codec/gpuencodepump.h
  // Owns a dedicated encode thread for the recorder StreamWorker. processEncoderTick
  // enqueues a job and returns immediately (never blocking the master pull clock).
  // The pump thread, per job: waits the GpuFence to the job's producer fence value
  // (FENCE-BEFORE-ENCODE), then encodeSurface()s the held FrameHandle's GpuSurface,
  // forwarding packets through the same PacketCallback the synchronous path uses.
  // The job RETAINS the FrameHandle so its surface outlives the async encode.
  class GpuEncodePump {
  public:
      // onPacket runs on the PUMP thread; the StreamWorker callback writes to the
      // muxer, which is only touched from the encode path, so this is the encode
      // thread of record (the CPU path's encode also ran on the tick thread — the
      // pump moves encode off the tick thread; muxer access stays single-writer).
      using PacketSink = std::function<void(const QByteArray& data, int64_t ptsTicks, bool keyframe)>;

      GpuEncodePump(NativeVideoEncoder* encoder, std::shared_ptr<GpuFence> fence, int maxQueue = 4);
      ~GpuEncodePump();  // stops the thread, drains in-flight

      GpuEncodePump(const GpuEncodePump&) = delete;
      GpuEncodePump& operator=(const GpuEncodePump&) = delete;

      // Enqueue one GPU-resident frame for encode. fenceValue is the producer's
      // decode-done/render value the pump waits for BEFORE encoding. Returns false
      // (and bumps queueDrops()) if the bounded queue is full — the caller treats
      // that as a dropped CFR frame (repeat last), never blocking. NON-BLOCKING.
      bool submit(FrameHandle frame, uint64_t fenceValue, int64_t ptsTicks,
                  ColorMetadata color, PacketSink onPacket);

      void start();
      void stop();  // idempotent; joins the thread

      uint64_t queueDrops() const;     // bounded-queue overflow count
      uint64_t framesEncoded() const;  // successful encodeSurface calls
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpuencodepump.cpp` — drive the pump with a fake encoder (no real GPU) to prove the contract deterministically: submit waits the fence before encode, the queue drops oldest on overflow, and `framesEncoded`/`queueDrops` advance:

```cpp
// GpuEncodePump runs encode on a dedicated thread, fence-gated BEFORE each encode,
// without blocking the submitter (the master pull clock). A fake encoder + a real
// GpuFence (stub on CI) prove: (a) no encode happens until the fence reaches the
// job's value; (b) submit never blocks and drops the oldest on overflow.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "recorder_engine/codec/gpuencodepump.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#include <atomic>

namespace {
// A surface-accepting fake that records encodeSurface calls; the other methods
// are unused by the pump.
class FakeEncoder : public NativeVideoEncoder {
public:
    std::atomic<int> calls{0};
    bool encode(const AVFrame*, int64_t, const PacketCallback&, QString*) override { return false; }
    bool encodeSurface(GpuSurface*, int64_t pts, const ColorMetadata&,
                       const PacketCallback& onPacket, QString*) override {
        calls.fetch_add(1, std::memory_order_acq_rel);
        onPacket(QByteArray("pkt"), pts, true);
        return true;
    }
    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArray("avcc"); }
};

// Minimal valid GpuSurface so the FrameHandle is GPU-shaped (the fake encoder
// ignores the actual pixels).
class FakeSurface : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return const_cast<FakeSurface*>(this); }
};
}  // namespace

class TestGpuEncodePump : public QObject {
    Q_OBJECT
private slots:
    void encodeWaitsForFenceThenForwardsPacket();
    void submitDropsOldestOnOverflowNeverBlocks();
};

void TestGpuEncodePump::encodeWaitsForFenceThenForwardsPacket() {
    FakeEncoder enc;
    auto fence = GpuFence::create();
    QVERIFY(fence != nullptr);  // stub fence exists on every host
    GpuEncodePump pump(&enc, fence, 4);
    pump.start();

    auto surface = std::make_shared<FakeSurface>();
    FrameHandle h(std::make_shared<GpuFrameData>(surface, nullptr, FramePixelFormat::Nv12),
                  FrameMetadata{});
    std::atomic<int> packets{0};
    // Require fence value 1; do NOT signal yet -> the pump must not encode.
    QVERIFY(pump.submit(h, 1, 100, ColorMetadata{},
                        [&](const QByteArray&, int64_t, bool) { packets.fetch_add(1); }));
    QTest::qWait(60);
    QCOMPARE(enc.calls.load(), 0);  // FENCE-BEFORE-ENCODE: still gated
    fence->signal();                // reaches value 1
    QTRY_COMPARE_WITH_TIMEOUT(enc.calls.load(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(packets.load(), 1, 2000);
    pump.stop();
    QCOMPARE(pump.framesEncoded(), uint64_t(1));
}

void TestGpuEncodePump::submitDropsOldestOnOverflowNeverBlocks() {
    FakeEncoder enc;
    auto fence = GpuFence::create();
    GpuEncodePump pump(&enc, fence, 2);  // tiny queue
    // Do NOT start the thread, so jobs accumulate and overflow deterministically.
    auto surface = std::make_shared<FakeSurface>();
    FrameHandle h(std::make_shared<GpuFrameData>(surface, nullptr, FramePixelFormat::Nv12),
                  FrameMetadata{});
    auto noop = [](const QByteArray&, int64_t, bool) {};
    QVERIFY(pump.submit(h, 9, 1, ColorMetadata{}, noop));
    QVERIFY(pump.submit(h, 9, 2, ColorMetadata{}, noop));
    // Third submit overflows the depth-2 queue: returns false, drops oldest, never blocks.
    QVERIFY(!pump.submit(h, 9, 3, ColorMetadata{}, noop));
    QCOMPARE(pump.queueDrops(), uint64_t(1));
}

QTEST_GUILESS_MAIN(TestGpuEncodePump)
#include "tst_gpuencodepump.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:

```cmake
    olr_add_unit_test(tst_gpuencodepump olr_test_engine olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpuencodepump
```
Expected: FAIL to compile — `recorder_engine/codec/gpuencodepump.h` not found.

- [ ] **Step 3: Write the implementation**

Create `recorder_engine/codec/gpuencodepump.h` (interface above) and `gpuencodepump.cpp`. The job struct holds `{FrameHandle frame; uint64_t fenceValue; int64_t ptsTicks; ColorMetadata color; PacketSink onPacket;}`. The thread loop pops a job under a mutex (condition-variable wait), releases the lock, waits the fence (`m_fence->wait(job.fenceValue, kFenceTimeoutMs)`), then `m_encoder->encodeSurface(job.frame.data()->gpuSurface(), job.ptsTicks, job.color, job.onPacket, &err)`:

```cpp
#include "recorder_engine/codec/gpuencodepump.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "recorder_engine/codec/nativevideoencoder.h"

namespace {
constexpr int kFenceTimeoutMs = 100;  // bounded; a missed fence drops the frame, never hangs
}

GpuEncodePump::GpuEncodePump(NativeVideoEncoder* encoder, std::shared_ptr<GpuFence> fence,
                             int maxQueue)
    : m_encoder(encoder), m_fence(std::move(fence)), m_maxQueue(maxQueue) {}

GpuEncodePump::~GpuEncodePump() { stop(); }

void GpuEncodePump::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void GpuEncodePump::stop() {
    if (!m_running.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    { std::lock_guard<std::mutex> lk(m_mutex); m_cv.notify_all(); }
    if (m_thread.joinable()) m_thread.join();
}

bool GpuEncodePump::submit(FrameHandle frame, uint64_t fenceValue, int64_t ptsTicks,
                           ColorMetadata color, PacketSink onPacket) {
    std::lock_guard<std::mutex> lk(m_mutex);
    bool dropped = false;
    if (int(m_queue.size()) >= m_maxQueue) {
        m_queue.pop_front();  // drop oldest: a stale CFR frame the operator never sees
        m_drops.fetch_add(1, std::memory_order_acq_rel);
        dropped = true;
    }
    // The job RETAINS the FrameHandle (refcount bump) so its GpuSurface outlives
    // the async encode (surface-lifetime contract, spec §4).
    m_queue.push_back(Job{std::move(frame), fenceValue, ptsTicks, color, std::move(onPacket)});
    m_cv.notify_one();
    return !dropped;
}

void GpuEncodePump::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] { return !m_running.load() || !m_queue.empty(); });
            if (!m_running.load() && m_queue.empty()) return;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        // FENCE-BEFORE-ENCODE: never read a surface the producer is still writing.
        // Runs on the pump thread, NEVER the master pull clock -> the tick can't stall.
        if (m_fence && !m_fence->wait(job.fenceValue, kFenceTimeoutMs)) {
            m_drops.fetch_add(1, std::memory_order_acq_rel);  // fence missed: drop, don't hang
            continue;
        }
        GpuSurface* surface = job.frame.data() ? job.frame.data()->gpuSurface() : nullptr;
        if (!surface) continue;
        QString err;
        if (m_encoder->encodeSurface(surface, job.ptsTicks, job.color, job.onPacket, &err))
            m_encoded.fetch_add(1, std::memory_order_acq_rel);
    }
}

uint64_t GpuEncodePump::queueDrops() const { return m_drops.load(std::memory_order_acquire); }
uint64_t GpuEncodePump::framesEncoded() const { return m_encoded.load(std::memory_order_acquire); }
```
Members in the header: `NativeVideoEncoder* m_encoder; std::shared_ptr<GpuFence> m_fence; int m_maxQueue; std::thread m_thread; std::mutex m_mutex; std::condition_variable m_cv; std::deque<Job> m_queue; std::atomic<bool> m_running{false}; std::atomic<uint64_t> m_drops{0}, m_encoded{0};` and a private `void run();` plus the `struct Job`.

Add `gpuencodepump.cpp` to `olr_test_engine` (`tests/CMakeLists.txt`) and the production `OpenLiveReplay` recorder sources (`CMakeLists.txt`) inside the `if(OLR_GPU_PIPELINE)` block — it `#include`s `playback/gpu/gpufence.h`, so it compiles only with the GPU pipeline on (guard the source-add with the option, mirroring how `gpuframedata.cpp` is added).

**Review gate:** the pump thread + bounded queue + the `FrameHandle`-retains-surface contract are the TSan-relevant surface. The reviewer verifies: (a) `submit` never blocks (no fence-wait, no encode under the submit lock); (b) the fence-wait runs on the pump thread only; (c) the job holds the `FrameHandle` so `gpuSurface()` stays valid through `encodeSurface`; (d) `stop()` drains/joins cleanly with no in-flight encode reading a freed surface. Flag for independent review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpuencodepump && ctest --test-dir build/c -R tst_gpuencodepump --output-on-failure
```
Expected: PASS (2 tests; deterministic — uses the stub `GpuFence` and a fake encoder, no real GPU).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/codec/gpuencodepump.h recorder_engine/codec/gpuencodepump.cpp \
        tests/unit/tst_gpuencodepump.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-encode): GpuEncodePump (async encode thread, fence-gated before encode)"
```

---

## Task 5: Wire the pump into `StreamWorker::processEncoderTick` (GPU-input branch)

**Precondition:** Tasks 3-4. **Grounds in the REAL dispatch site — `streamworker.cpp:228-274`.**

**Files:**
- Modify: `recorder_engine/streamworker.h` (members: `m_gpuEncodePump`, `m_gpuEncodeFence`, the GPU-input held-handle), `recorder_engine/streamworker.cpp` (`setupEncoder` constructs the pump under the flag; `processEncoderTick` branches to the pump when the held frame is GPU-backed; `run()` stops the pump on teardown)
- Test: `tests/unit/tst_streamworker_gpuencode.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuEncodePump` (Task 4), `GpuFence`/`makeD3D11GpuFence` (`playback/gpu/gpufence.h`), `FrameHandle`/`GpuSurface` (the GPU-resident held frame), `gpuPipelineEnabled()` (`playback/gpu/gpupipelineconfig.h`), the existing `m_nativeEncoder`/`m_muxer`/`m_internalFrameCount`/`m_viewTrack` (`streamworker.h`).
- Produces (ADD to `StreamWorker`, all `#ifdef OLR_GPU_PIPELINE_BUILD`):
  ```cpp
  // recorder_engine/streamworker.h (private):
  #ifdef OLR_GPU_PIPELINE_BUILD
      std::unique_ptr<GpuEncodePump> m_gpuEncodePump;     // null unless gpuPipelineEnabled()
      std::shared_ptr<GpuFence> m_gpuEncodeFence;          // the producer/encode fence
      FrameHandle m_latestGpuFrame;                        // GPU-resident held frame (tick thread)
      uint64_t m_latestGpuFenceValue = 0;                  // its decode-done/render value
  #endif
  // The muxer-write callback factory shared by the CPU and GPU encode paths (the
  // existing inline lambda at streamworker.cpp:254-273 extracted so the pump reuses
  // it verbatim). Captures the track + stream + muxer; writes each packet.
  NativeVideoEncoder::PacketCallback makeMuxerWriteCallback(int track, AVStream* st,
                                                            bool* havePacket);
  ```
  The GPU held frame is supplied to the worker by the GPU-resident ingest path (the same keep-surface decode that the playback worker uses); the wiring point in `processEncoderTick` reads `m_latestGpuFrame` instead of `m_latestFrame` when it is non-null and `gpuPipelineEnabled()`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_streamworker_gpuencode.cpp` — a focused unit that does NOT spin up a real source. It exercises the dispatch decision via a thin test seam: confirm that with the flag off `processEncoderTick`'s GPU members are inert (the pump is null), and that the shared muxer-write callback writes a packet identically for a GPU and a CPU packet. Because `StreamWorker` is a `QThread` with a private dispatch, expose a `friend`-class seam (the repo already uses `OLR_UNIT_TEST` friend seams — `tests/CMakeLists.txt` defines `OLR_UNIT_TEST=1` on `olr_test_engine`) or test the extracted `makeMuxerWriteCallback` directly:

```cpp
// The StreamWorker GPU-encode wiring: with the flag off the pump is never built
// (the recorder is byte-identical to the CPU path); the muxer-write callback is
// shared verbatim between the CPU AVFrame path and the GPU pump path.
#include <QtTest>

#include "recorder_engine/streamworker.h"

class TestStreamWorkerGpuEncode : public QObject {
    Q_OBJECT
private slots:
    void pumpIsNullWhenPipelineFlagOff();
};

void TestStreamWorkerGpuEncode::pumpIsNullWhenPipelineFlagOff() {
    qunsetenv("OLR_GPU_PIPELINE");  // flag off
    // Construct a worker with no muxer/clock (we never run it); assert the GPU
    // encode pump is not constructed at setup time under the off flag. The seam
    // is StreamWorker::gpuEncodePumpForTest() (OLR_UNIT_TEST-only accessor).
    StreamWorker w(QString(), 0, /*muxer*/ nullptr, /*clock*/ nullptr, 320, 240, 30, 30, 1,
                   VideoCodecChoice::H264Hardware);
    QVERIFY(w.gpuEncodePumpForTest() == nullptr);
}

QTEST_GUILESS_MAIN(TestStreamWorkerGpuEncode)
#include "tst_streamworker_gpuencode.moc"
```

Register in `tests/unit/CMakeLists.txt` (unconditional — it asserts the off-path; under `OLR_GPU_PIPELINE` it additionally compiles the GPU members):

```cmake
olr_add_unit_test(tst_streamworker_gpuencode olr_test_engine)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_streamworker_gpuencode
```
Expected: FAIL to compile — `gpuEncodePumpForTest()` is not a member of `StreamWorker`.

- [ ] **Step 3: Write the implementation**

In `streamworker.h`, under `#ifdef OLR_UNIT_TEST` add `const GpuEncodePump* gpuEncodePumpForTest() const;` returning `m_gpuEncodePump.get()` (or `nullptr` when the GPU build is off). Add the `#ifdef OLR_GPU_PIPELINE_BUILD` members above and `#include "recorder_engine/codec/gpuencodepump.h"` / `"playback/gpu/gpufence.h"` / `"playback/output/framehandle.h"` under the same guard.

In `streamworker.cpp`:
1. **Extract the muxer-write callback.** Refactor the inline lambda at `:254-273` into `makeMuxerWriteCallback(track, st, &havePacket)` returning a `NativeVideoEncoder::PacketCallback` with identical body (allocate `AVPacket`, copy `data`, rescale PTS, `m_muxer->writePacket`, set `havePacket`). The CPU path calls `m_nativeEncoder->encode(m_latestFrame, m_internalFrameCount, makeMuxerWriteCallback(...), &encErr)` — byte-identical behavior.
2. **Construct the pump in `setupEncoder`** (after `m_nativeEncoder` is created, `:558-564`), under the flag:
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
       if (gpuPipelineEnabled()) {
           m_gpuEncodeFence = GpuFence::create();  // Apple; Windows uses makeD3D11GpuFence(edge device)
           m_gpuEncodePump = std::make_unique<GpuEncodePump>(m_nativeEncoder.get(), m_gpuEncodeFence);
           m_gpuEncodePump->start();
       }
   #endif
   ```
3. **Branch the dispatch** in `processEncoderTick` (`:247`). Where the CPU path checks `m_videoCodec == VideoCodecChoice::H264Hardware && m_nativeEncoder`, add a GPU pre-check: when `gpuPipelineEnabled()` and `m_latestGpuFrame` is non-null and GPU-backed, submit to the pump and return (the pump writes packets through the same callback); otherwise fall through to the unchanged CPU `encode(m_latestFrame, ...)` path:
   ```cpp
   #ifdef OLR_GPU_PIPELINE_BUILD
       if (m_gpuEncodePump && !m_latestGpuFrame.isNull() && m_latestGpuFrame.isGpuBacked()) {
           AVStream* st = m_muxer->getStream(track);
           bool dummyHave = false;  // the pump writes inline; havePacket is the CPU path's bookkeeping
           m_gpuEncodePump->submit(m_latestGpuFrame, m_latestGpuFenceValue, m_internalFrameCount,
                                   m_latestGpuFrame.metadata().color, makeMuxerWriteCallback(track, st, &dummyHave));
           // Audio + metadata still flow through the existing tick tail (writeAudioForTick),
           // exactly as the CPU path; only the video encode moved to the pump.
       } else
   #endif
       if (m_videoCodec == VideoCodecChoice::H264Hardware && m_nativeEncoder) {
           // ... unchanged CPU AVFrame encode path ...
       }
   ```
   (The metadata/audio tail at `:295-328` is unchanged. The TC-candidate registration at `:240-245` stays before the branch.)
4. **Stop the pump on teardown** in `run()` (where `m_nativeEncoder.reset()` is, `:112`), under the flag: `if (m_gpuEncodePump) m_gpuEncodePump->stop();` before resetting the encoder.

The GPU held frame (`m_latestGpuFrame` / `m_latestGpuFenceValue`) is populated by the GPU-resident ingest path in a follow-up integration (the keep-surface decode produces a GPU-backed `FrameHandle` + its decode-done fence value); this task wires the **dispatch seam** so the pump is fed the instant a GPU frame is held. Until that producer is wired, `m_latestGpuFrame` stays null and the CPU path runs — preserving the zero-regression gate.

**Review gate:** `processEncoderTick` runs on the tick (master-pulse) thread. The reviewer verifies the GPU branch only **enqueues** (no fence-wait, no encode on the tick thread), and the audio/metadata/TC tail is unchanged. Flag for independent review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_streamworker_gpuencode && ctest --test-dir build/c -R tst_streamworker_gpuencode --output-on-failure
```
Expected: PASS (pump null with the flag off).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
# Off-path byte-green (fresh dir): the recorder must be byte-identical with the flag off.
cmake -S . -B build/off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF
cmake --build build/off && ctest --test-dir build/off -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp tests/unit/tst_streamworker_gpuencode.cpp tests/unit/CMakeLists.txt
git commit -m "feat(gpu-encode): wire GpuEncodePump into StreamWorker dispatch (GPU-input branch, flag-gated)"
```

---

## Task 6: Capstone — decode-GPU → encode-GPU stays-GPU round-trip (`tst_h264_roundtrip` GPU variant)

**Precondition:** Tasks 1-5; a VideoToolbox encoder + decoder + `decodeKeepSurface` on the host (else `QSKIP`). **This is the unblock-the-redesign proof.**

**Files:**
- Modify: `tests/unit/tst_h264_roundtrip.cpp` (add the GPU-input round-trip slot)
- Modify: `tests/unit/CMakeLists.txt` (link the GPU surface fixture + `olr_test_playback`/`olr_test_gpu` to `tst_h264_roundtrip` under `OLR_GPU_PIPELINE`)

**Interfaces:**
- Consumes: `NativeVideoEncoder::create`/`encode`/`encodeSurface`/`avccExtradata` (Tasks 2-3), `NativeVideoDecoder`/`decodeKeepSurface` (`recorder_engine/ingest/nativevideodecoder.h`), `importVtSurface`/`importVtImageBuffer` (`playback/gpu/vtkeepsurfaceimporter.h`), `GpuSurface`/`FrameHandle`, `parseAvcc` (`recorder_engine/codec/avcc.h`), `psnrY8` (`tests/unit/framepsnr.h`).
- Produces: no new product interface — a behavioral gate proving the full GPU round-trip matches the CPU-upload path within PSNR tolerance.

- [ ] **Step 1: Write the failing test**

Add a slot `decodeGpuEncodeGpuRoundTripMatchesCpuUpload()` to `tst_h264_roundtrip.cpp`. It (1) encodes the existing `makePattern()` source via the CPU-upload `encode` to produce a one-keyframe MKV (the reference), (2) demuxes the first keyframe and `decodeKeepSurface`s it to an IOSurface-backed `CVImageBufferRef` wrapped as a GPU-backed `FrameHandle` via `importVtImageBuffer`, (3) `encodeSurface`s that surface directly (zero re-upload) into a second MKV, (4) decodes BOTH outputs back to CPU `AVFrame`s, and (5) asserts the GPU-path decode matches the source within the SAME PSNR floors as the CPU path AND that the GPU-path luma PSNR is within a tolerance of the CPU-path luma PSNR (the two encode routes must produce equivalent quality):

```cpp
void TestH264RoundTrip::decodeGpuEncodeGpuRoundTripMatchesCpuUpload() {
#ifndef __APPLE__
    QSKIP("GPU round-trip capstone runs on the VideoToolbox host path");
#else
    if (!queryNativeVideoDecodeCapabilities().h264) QSKIP("no HW H.264 decoder");
    QString err;
    auto enc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no HW H.264 encoder");
    AVFrame* source = makePattern();           // existing helper
    auto freeSource = qScopeGuard([&] { av_frame_free(&source); });

    // (1) Reference: CPU-upload encode -> keyframe + avcC. (encode one frame.)
    bool got = false; QByteArray refPacketAnnexB; // capture the keyframe NALs
    QVERIFY(enc->encode(source, 0, [&](const QByteArray& d, int64_t, bool key) {
        if (!d.isEmpty() && key && refPacketAnnexB.isEmpty()) refPacketAnnexB = d; got = true; }, &err));
    if (!got) QSKIP("encoder produced no keyframe");
    const QByteArray avcc = enc->avccExtradata();
    QList<QByteArray> sps, pps; QVERIFY(parseAvcc(avcc, &sps, &pps));

    // (2) Decode the reference keyframe KEEPING the IOSurface; wrap as a GPU FrameHandle.
    H26xParameterSets ps; ps.h264Sps = sps; ps.h264Pps = pps;
    CompressedAccessUnit unit; unit.codec = NativeVideoCodec::H264;
    unit.parameterSets = ps; unit.pts90k = 0; unit.dts90k = 0;
    unit.annexB = lengthPrefixedToAnnexB(refPacketAnnexB);  // test-local helper (avcC->AnnexB)
    NativeVideoDecoder dec(640, 480);
    FrameHandle gpuFrame;
    bool keptSurface = dec.decodeKeepSurface(unit, [&](void* cvImageBuffer, qint64) {
        FrameMetadata meta; meta.key.format = FramePixelFormat::Nv12; meta.key.width = 640; meta.key.height = 480;
        gpuFrame = importVtImageBuffer(cvImageBuffer, meta, /*rhi*/ nullptr);
    }, &err);
    if (!keptSurface || gpuFrame.isNull() || !gpuFrame.isGpuBacked())
        QSKIP("keep-surface decode did not yield a GPU-backed handle on this host");

    // (3) Encode the GPU surface DIRECTLY (zero re-upload). The producer is done,
    // so no fence-wait is required here (synchronous decode already completed).
    auto enc2 = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    QVERIFY(enc2 != nullptr);
    QByteArray gpuPacketAnnexB;
    QVERIFY(enc2->encodeSurface(gpuFrame.data()->gpuSurface(), 0, gpuFrame.metadata().color,
        [&](const QByteArray& d, int64_t, bool key) { if (!d.isEmpty() && key && gpuPacketAnnexB.isEmpty()) gpuPacketAnnexB = d; }, &err));
    QVERIFY2(!gpuPacketAnnexB.isEmpty(), qPrintable(err));
    QList<QByteArray> sps2, pps2; QVERIFY(parseAvcc(enc2->avccExtradata(), &sps2, &pps2));

    // (4) Decode BOTH back to CPU and PSNR-compare against the source pattern.
    const double cpuLuma = decodeKeyframeLumaPsnr(refPacketAnnexB, sps, pps, source);   // helpers
    const double gpuLuma = decodeKeyframeLumaPsnr(gpuPacketAnnexB, sps2, pps2, source);
    qInfo("round-trip luma PSNR: cpu-upload=%.2f dB  gpu-direct=%.2f dB", cpuLuma, gpuLuma);

    // (5) The GPU-direct route must clear the same quality floor AND track the CPU
    // route within a tolerance (the two encode paths feed equivalent pixels).
    constexpr double kMinLumaPsnrDb = 40.0;
    QVERIFY2(gpuLuma >= kMinLumaPsnrDb, "GPU-direct encode dropped below the luma PSNR floor");
    QVERIFY2(std::abs(gpuLuma - cpuLuma) <= 3.0,
             "GPU-direct and CPU-upload encode diverge beyond tolerance");
#endif
}
```

Add the small test-local helpers (`lengthPrefixedToAnnexB`, `decodeKeyframeLumaPsnr`) above the class, reusing the avcC↔AnnexB conversion already inlined in `encodeMuxDemuxYieldsIntraH264` (:217-230) and `psnrY8` (`framepsnr.h`). Include `playback/gpu/vtkeepsurfaceimporter.h`, `playback/gpu/gpusurface.h`, `playback/output/framehandle.h`, and `recorder_engine/ingest/nativevideodecoder.h`.

In `tests/unit/CMakeLists.txt`, under `if(OLR_GPU_PIPELINE)` add the GPU libs to `tst_h264_roundtrip` so the capstone can link the surface importer + decoder:
```cmake
if(OLR_GPU_PIPELINE)
    target_link_libraries(tst_h264_roundtrip PRIVATE olr_test_nativevideodecoder olr_test_playback olr_test_gpu)
    target_compile_definitions(tst_h264_roundtrip PRIVATE OLR_GPU_PIPELINE_BUILD=1)
endif()
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_h264_roundtrip
```
Expected: FAIL to compile — `importVtImageBuffer`/`decodeKeepSurface`/`encodeSurface` symbols unresolved until the GPU libs are linked, or the new helpers are missing.

- [ ] **Step 3: Write the implementation**

This task is test-only — the product code already exists (Tasks 2-3 + the merged `decodeKeepSurface`/`importVtImageBuffer`). Add the test-local helpers and the CMake link/define edits above. No production change.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_h264_roundtrip && ctest --test-dir build/c -R tst_h264_roundtrip --output-on-failure
```
Expected: PASS on a VideoToolbox host (the existing `encodeMuxDemuxYieldsIntraH264` + `encodedSpsCarriesRequestedColorVui` + `decodeGpuEncodeGpuRoundTripMatchesCpuUpload`). On a host without keep-surface support the capstone `QSKIP`s.

- [ ] **Step 5: Full pre-flight + final review**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
OLR_GPU_PIPELINE=1 ctest --test-dir build/c -R "tst_gpu_encode_surface|tst_gpuencodepump|tst_h264_roundtrip|tst_colorvui" --output-on-failure
cmake --build build/off && ctest --test-dir build/off -L unit --output-on-failure   # off-path byte-green
```
Expected: GPU-on and GPU-off suites both PASS; the recorder is byte-identical with the flag off.

**Review gate (final, whole-subproject):** per CLAUDE.md, the recorder worker's threading + the encode-pump changes get an independent fresh-agent concurrency review before merge. The reviewer verifies: (a) `processEncoderTick` never blocks on a fence or an encode (grep the GPU branch — only `submit`); (b) every `encodeSurface` reachable from the pump is preceded by `GpuFence::wait` on the pump thread (FENCE-BEFORE-ENCODE); (c) the pump's job retains the `FrameHandle` so the surface outlives the async encode; (d) with the flag off, `makeI420PixelBuffer` + the CPU `encode` path is byte-identical to the merged recorder. Open the PR with the per-branch push:
```sh
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin gpu-phase5-gpu-encode
```

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add tests/unit/tst_h264_roundtrip.cpp tests/unit/CMakeLists.txt
git commit -m "test(gpu-encode): capstone decode-GPU -> encode-GPU round-trip vs CPU-upload (PSNR parity)"
```

---

## Canonical gpu-encode contract (for the recorder-engine redesign that consumes this)

`gpu-encode` adds exactly these names; the recorder redesign and any future GPU output path build on them verbatim.

**Color VUI mapping** — `recorder_engine/codec/colorvui.h` (NEW):
```cpp
struct VuiColorCodePoints { int colourPrimaries=1, transferCharacteristics=1, matrixCoefficients=1; bool fullRange=false; };
VuiColorCodePoints vuiColorCodePointsFor(const ColorMetadata& color);
```

**GPU-surface encode** — `recorder_engine/codec/nativevideoencoder.h` (existing class, ADDED):
```cpp
struct Config { int width, height, fpsNum, fpsDen, bitrate; ColorMetadata color; };   // color ADDED
virtual bool encodeSurface(GpuSurface* surface, int64_t ptsTicks, const ColorMetadata& color,
                           const PacketCallback& onPacket, QString* error) = 0;        // NEW
```

**Async encode pump** — `recorder_engine/codec/gpuencodepump.h` (NEW):
```cpp
class GpuEncodePump {
    GpuEncodePump(NativeVideoEncoder* encoder, std::shared_ptr<GpuFence> fence, int maxQueue = 4);
    bool submit(FrameHandle frame, uint64_t fenceValue, int64_t ptsTicks, ColorMetadata color, PacketSink onPacket); // NON-BLOCKING
    void start(); void stop();
    uint64_t queueDrops() const; uint64_t framesEncoded() const;
};
```

**StreamWorker wiring** — `recorder_engine/streamworker.h` (NEW, `OLR_GPU_PIPELINE_BUILD`):
```cpp
std::unique_ptr<GpuEncodePump> m_gpuEncodePump;
std::shared_ptr<GpuFence>      m_gpuEncodeFence;
FrameHandle                    m_latestGpuFrame;       // GPU-resident held frame (tick thread)
uint64_t                       m_latestGpuFenceValue;  // its decode-done/render value
NativeVideoEncoder::PacketCallback makeMuxerWriteCallback(int track, AVStream* st, bool* havePacket);
```
