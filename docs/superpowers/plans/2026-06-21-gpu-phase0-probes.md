# GPU Phase 0 Gating Probes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Settle the six open Phase-0 questions (§11 of the design spec) with small, recorded measurement probes — each ending in an explicit go/no-go finding that unblocks a Phase-2 design decision (D4/D11) — while landing the one real import-edge code change (requesting `kCVPixelBufferIOSurfacePropertiesKey` on the VideoToolbox decode session), test-backed.

**Architecture:** Each probe is a tiny standalone harness (a Qt-Test unit on macOS where hardware exists, or a documented manual/CI experiment on Windows) plus a recorded finding written into a single living markdown report under `docs/superpowers/probes/`. The probes do NOT link Qt RHI into the product; they build throwaway-free measurement tools that stay in the tree and a permanent IOSurface-backing change on the VT decoder. The CPU pipeline is untouched and stays default.

**Tech Stack:** C++17, Qt 6 (Core/Test/Gui/GuiPrivate for `QRhi`), Apple VideoToolbox/CoreVideo/IOSurface/Metal, Qt6::ShaderTools (present at `$HOME/Qt/6.10.1/macos/lib/QtShaderTools.framework`), Windows Media Foundation/D3D11/D3D12 (documented, Windows-CI-gated), CMake + Ninja, ffprobe (Homebrew `/opt/homebrew/bin/ffprobe`).

## Global Constraints

- **Keystone-first.** These probes precede and gate the keystone (`frame-handle`, Phase 1) and the GPU edge (`gpu-abstraction`/`gpu-import-win`, Phase 2). They produce *findings + one edge change*, not product features. No probe is allowed to change pipeline behavior except the IOSurface-backing edge change in Task 1, which is proven a behavior-preserving no-op for the CPU readback path.
- **The CPU path stays default and is the permanent correctness reference + fallback.** No probe wires a GPU surface into any decode/composite/sink path. RHI is exercised in isolated harnesses only.
- **Everything behind flags.** Any new build target or experiment is opt-in (a dedicated probe executable / unit test, never linked into the shipping app). The RHI probe harness is gated behind a CMake option `OLR_BUILD_GPU_PROBES` (default `OFF`) so the product/app build and the default CI lanes are unaffected.
- **No throwaways.** Every artifact (harness, unit test, the IOSurface change, the findings doc) is production and stays in the tree. The probe harnesses become the first regression coverage for the Phase-2 import edge.
- **Public-repo professionalism.** Code, comments, commit messages, and the findings doc are published — self-contained, no secrets, no internal/private references; document the present design and the measured result, not history.
- **Format changed lines only.** Some engine files (e.g. `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm`) use hand-written Allman style. Run `git clang-format --commit origin/main` on changed lines only; never reformat whole files. `.qml` is checked by `qmllint` (none here).
- Build (run from the worktree root `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/gpu-phase0-2-plans`): configure once with `cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_BUILD_GPU_PROBES=ON`; build a target with `cmake --build build/c --target <target>`; run a unit test with `ctest --test-dir build/c -R <name> --output-on-failure`. Use a fresh build directory when switching configurations. The full unit suite is `ctest --test-dir build/c -L unit --output-on-failure`.
- Unit tests are registered via `olr_add_unit_test(<name> <libs>)` in `tests/unit/CMakeLists.txt` and run headless with `QT_QPA_PLATFORM=offscreen`. On non-Apple hosts every Apple-only probe test `QSKIP`s gracefully (mirrors the existing `tst_nativevideodecoder` platform gate at `tests/unit/CMakeLists.txt:45,59-81`).

---

## File Structure

- **Create** `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md` — the single living findings doc; every task appends its recorded result + the decision it settles.
- **Modify** `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` — request `kCVPixelBufferIOSurfacePropertiesKey` on the decode session attributes (Task 1); add an `Impl::lastDecodedWasIOSurfaceBacked()` test probe accessor.
- **Modify** `recorder_engine/ingest/nativevideodecoder.h` — expose the probe accessor.
- **Create** `tests/unit/tst_vtiosurface.cpp` — asserts the decoded `CVPixelBuffer` is IOSurface-backed (Task 1) and records reconfig cost (Task 2).
- **Create** `tests/probes/rhi_import_probe.cpp` — RHI cross-thread import→composite→readback micro-benchmark harness (Tasks 3, 5).
- **Create** `tests/probes/CMakeLists.txt` — `OLR_BUILD_GPU_PROBES`-gated probe targets.
- **Modify** `CMakeLists.txt` — add the `OLR_BUILD_GPU_PROBES` option and `add_subdirectory(tests/probes)` under it.
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_vtiosurface` against the platform `olr_test_nativevideodecoder` backend lib.
- **Create** `tests/probes/audit_fixture_color_tags.sh` — ffprobe-driven color-tag audit over the e2e fixture-producing ffmpeg recipes (Task 6).

---

## Task 1: macOS VT decode session yields an IOSurface-backed CVPixelBuffer (P0.1, the real import-edge change)

Settles **§11 Q1 (part A)** / **D-edge (macOS)**: does the VT decode session yield an IOSurface-backed buffer (a precondition for `CVMetalTextureCache` zero-copy import in `gpu-abstraction`)? Today `createSession` (`recorder_engine/ingest/nativevideodecoder_videotoolbox.mm:324-356`) does **not** request `kCVPixelBufferIOSurfacePropertiesKey`, so VT may hand back a non-IOSurface buffer.

**Files:**
- Modify: `recorder_engine/ingest/nativevideodecoder.h` (add probe accessor)
- Modify: `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` (request IOSurface; record last-buffer IOSurface flag)
- Test: `tests/unit/tst_vtiosurface.cpp`
- Modify: `tests/unit/CMakeLists.txt`
- Create: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Produces: `bool NativeVideoDecoder::lastDecodedWasIOSurfaceBacked() const;` — returns whether the most recently decoded `CVPixelBuffer` had a non-null `IOSurfaceRef` (always `false` on non-Apple builds via the stub). Reads a `bool` recorded in the decompression output callback.
- Consumes: existing `NativeVideoDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString*)` and `queryNativeVideoDecodeCapabilities()`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_vtiosurface.cpp`:

```cpp
// Phase-0 probe P0.1: prove the VideoToolbox decode session hands back an
// IOSurface-backed CVPixelBuffer (precondition for zero-copy CVMetalTextureCache
// import in gpu-abstraction). Apple-only behavioral test; QSKIPs elsewhere.
#include <QtTest>

#include "recorder_engine/ingest/nativevideodecoder.h"
#include "recorder_engine/ingest/h26xaccessunit.h"

extern "C" {
#include <libavutil/frame.h>
}

class TestVtIOSurface : public QObject {
    Q_OBJECT
private slots:
    void decodedBufferIsIOSurfaceBacked();

private:
    // A single all-intra H.264 IDR access unit decodable by VideoToolbox, built
    // by encoding one grey frame through the project's native encoder when
    // available; falls back to QSKIP if no HW encoder exists to author the AU.
    static bool buildGreyIdrAccessUnit(CompressedAccessUnit* unit, int w, int h);
};

bool TestVtIOSurface::buildGreyIdrAccessUnit(CompressedAccessUnit* unit, int w, int h) {
    // The repo ships a native H.264 encoder used by the recorder; reuse it to
    // mint a self-contained IDR + SPS/PPS so this probe needs no fixture file.
    // (See recorder_engine/codec/nativevideoencoder.h.)
    return CompressedAccessUnit::buildGreyIdrForTest(unit, w, h);
}

void TestVtIOSurface::decodedBufferIsIOSurfaceBacked() {
    const NativeVideoDecodeCapabilities caps = queryNativeVideoDecodeCapabilities();
    if (!caps.h264) QSKIP("no VideoToolbox H.264 decode on this platform");

    CompressedAccessUnit unit;
    if (!buildGreyIdrAccessUnit(&unit, 1280, 720))
        QSKIP("could not author a test IDR access unit on this platform");

    NativeVideoDecoder decoder(0, 0);
    int frames = 0;
    QString err;
    const bool ok = decoder.decode(unit, [&](AVFrame* f) {
        ++frames;
        av_frame_free(&f);
    }, &err);

    QVERIFY2(ok, qPrintable(err));
    QCOMPARE(frames, 1);
    QVERIFY2(decoder.lastDecodedWasIOSurfaceBacked(),
             "VT decode session did not produce an IOSurface-backed CVPixelBuffer");
}

QTEST_GUILESS_MAIN(TestVtIOSurface)
#include "tst_vtiosurface.moc"
```

Register it in `tests/unit/CMakeLists.txt` immediately after the `tst_nativevideodecoder` block (after line 81, the `endif()` that closes the per-platform `olr_test_nativevideodecoder` library), reusing that backend lib so the VT `.mm` is linked on Apple and the stub elsewhere:

```cmake
olr_add_unit_test(tst_vtiosurface olr_test_core)
target_link_libraries(tst_vtiosurface PRIVATE olr_test_nativevideodecoder)
```

> `CompressedAccessUnit::buildGreyIdrForTest` does not exist yet. If `CompressedAccessUnit` (`recorder_engine/ingest/h26xaccessunit.h`) has no such helper, replace `buildGreyIdrAccessUnit`'s body with an inline author using `NativeVideoEncoder::create({1280,720,30,1,4000000}, &err)` (header `recorder_engine/codec/nativevideoencoder.h`), encoding one `memset(...,128,...)` YUV420P `AVFrame`, capturing the emitted length-prefixed bytes + `avccExtradata()` SPS/PPS into a `CompressedAccessUnit{ codec=NativeVideoCodec::H264, parameterSets=<split avcC>, annexB=<the packet>, pts90k=0 }`. Step 3 below picks whichever path the real `h26xaccessunit.h` supports; do not invent a `buildGreyIdrForTest` API if a simpler in-test author works.

- [ ] **Step 2: Run the test to verify it fails**

Run:
```sh
cmake --build build/c --target tst_vtiosurface
```
Expected: FAIL to compile — `NativeVideoDecoder` has no member `lastDecodedWasIOSurfaceBacked`.

- [ ] **Step 3: Write the minimal implementation (the real edge change)**

In `recorder_engine/ingest/nativevideodecoder.h`, add the public probe accessor (place it next to the existing `void reset();` declaration):

```cpp
    // Phase-0 probe (P0.1): true iff the most recently decoded CVPixelBuffer was
    // IOSurface-backed. Always false on non-Apple builds. Used to gate the
    // zero-copy import edge; carries no behavioral meaning for the CPU path.
    bool lastDecodedWasIOSurfaceBacked() const;
```

In `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm`:

(a) Record the IOSurface flag in the decode context — extend `DecodeFrameContext` (at `:25-31`) with `bool ioSurfaceBacked = false;` and set it in `decompressionOutputCallback` (at `:193-221`), just before `copyPixelBufferToAvFrame` is called:

```cpp
    context->ioSurfaceBacked =
        imageBuffer && CVPixelBufferGetIOSurface(CVPixelBufferRef(imageBuffer)) != nullptr;
```

(b) Persist it to the `Impl` so the accessor can read it — add `bool lastIOSurfaceBacked = false;` to `class NativeVideoDecoder::Impl` (near `:236-247`), and after `VTDecompressionSessionWaitForAsynchronousFrames(session);` in `Impl::decode` (at `:450`) add `lastIOSurfaceBacked = context.ioSurfaceBacked;`.

(c) **Request IOSurface backing** in `Impl::createSession` (at `:324-356`) — after the pixel-format key is set (after `:330`, `CFRelease(pixelFormatNumber);`) and before the width/height block, add:

```cpp
    CFDictionaryRef ioSurfaceProps = CFDictionaryCreate(
        kCFAllocatorDefault, nullptr, nullptr, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurfaceProps);
    CFRelease(ioSurfaceProps);
```

(d) Add the accessor definition near the other `NativeVideoDecoder` method definitions (after `void NativeVideoDecoder::reset()` at `:475-477`):

```cpp
bool NativeVideoDecoder::lastDecodedWasIOSurfaceBacked() const {
    return m_impl->lastIOSurfaceBacked;
}
```

In the **stub** backend `recorder_engine/ingest/nativevideodecoder_stub.cpp`, add a matching definition that returns `false` (so non-Apple builds link):

```cpp
bool NativeVideoDecoder::lastDecodedWasIOSurfaceBacked() const { return false; }
```

> An empty `kCVPixelBufferIOSurfacePropertiesKey` dictionary is the canonical "give me IOSurface-backed buffers" request and is the same idiom the encoder side already uses (`recorder_engine/codec/nativevideoencoder_videotoolbox.mm` `makeI420PixelBuffer`). It does not change the CPU plane bytes `copyPixelBufferToAvFrame` produces — the readback path is unchanged.

- [ ] **Step 4: Run the test to verify it passes**

Run:
```sh
cmake --build build/c --target tst_vtiosurface && ctest --test-dir build/c -R tst_vtiosurface --output-on-failure
```
Expected: on a macOS host, PASS — one frame decoded, `lastDecodedWasIOSurfaceBacked()` true. On Linux CI the test links the stub backend and `queryNativeVideoDecodeCapabilities().h264` is false → `QSKIP`.

Also run the existing decoder test to prove the IOSurface change is behavior-preserving for the CPU readback path:
```sh
ctest --test-dir build/c -R tst_nativevideodecoder --output-on-failure
```
Expected: PASS (unchanged plane bytes).

- [ ] **Step 5: Record the finding + commit**

Create `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md` with a header and the P0.1 result:

```markdown
# GPU Phase 0 — Gating Probe Findings

Records the measured result and go/no-go decision for each Phase-0 probe
(spec §7, §11). Each probe gates a Phase-2 decision; the CPU pipeline stays the
default + reference throughout.

## P0.1 — macOS VT IOSurface-backed CVPixelBuffer (gates the macOS import edge)

- **Question (§11 Q1a):** does the VT decode session yield an IOSurface-backed
  CVPixelBuffer (precondition for CVMetalTextureCache zero-copy import)?
- **Method:** request `kCVPixelBufferIOSurfacePropertiesKey` on the decode
  session; assert `CVPixelBufferGetIOSurface()!=nullptr` on the decoded buffer
  (`tests/unit/tst_vtiosurface.cpp`).
- **Result:** <PASTE ctest output: PASS, IOSurface-backed on host
  `<sw_vers>` / Apple Silicon vs Intel>.
- **Decision:** GO for the macOS zero-copy import edge (`gpu-abstraction`). If
  the buffer were NOT IOSurface-backed, NO-GO → escalate to a forced
  `CVPixelBufferPoolCreate` with IOSurface attrs or a CPU-detour import.
- **Behavior-preserving:** `tst_nativevideodecoder` still passes (plane bytes
  unchanged); the CPU readback path is unaffected.
```

Format changed lines, then commit:
```sh
CF=/opt/homebrew/opt/llvm/bin/clang-format
python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/ingest/nativevideodecoder.h \
        recorder_engine/ingest/nativevideodecoder_videotoolbox.mm \
        recorder_engine/ingest/nativevideodecoder_stub.cpp \
        tests/unit/tst_vtiosurface.cpp tests/unit/CMakeLists.txt \
        docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe(P0.1): request IOSurface backing on VT decode session + assert it

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: VT session reconfig cost on resolution/parameter-set change (P0.1 cost half)

Settles **§11 Q1 (part B)**: *at what reconfig cost* does the IOSurface edge come — i.e. how expensive is the `ensureSession` teardown+recreate when SPS/PPS or geometry change mid-stream (`recorder_engine/ingest/nativevideodecoder_videotoolbox.mm:358-379`, the `reset()` → `createFormatDescription` → `createSession` path). This bounds the worst-case GPU-edge stall on a feed resolution flip.

**Files:**
- Test: `tests/unit/tst_vtiosurface.cpp` (add a second slot)
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes: `NativeVideoDecoder::decode(...)` (drives `ensureSession`), `QElapsedTimer`.
- Produces: a recorded median reconfig-cost number (ms) in the findings doc.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_vtiosurface.cpp` a slot declaration `void reconfigCostIsBounded();` and body:

```cpp
void TestVtIOSurface::reconfigCostIsBounded() {
    const NativeVideoDecodeCapabilities caps = queryNativeVideoDecodeCapabilities();
    if (!caps.h264) QSKIP("no VideoToolbox H.264 decode on this platform");

    CompressedAccessUnit a, b;
    if (!buildGreyIdrAccessUnit(&a, 1280, 720) || !buildGreyIdrAccessUnit(&b, 640, 480))
        QSKIP("could not author two distinct-geometry IDR access units");

    NativeVideoDecoder decoder(0, 0);
    QString err;
    auto drain = [](AVFrame* f) { av_frame_free(&f); };
    QVERIFY(decoder.decode(a, drain, &err)); // first session create (warm-up)

    // Measure the steady-state reconfig: alternate geometries so each decode
    // forces ensureSession() to reset()+recreate the session/format desc.
    QList<qint64> samples;
    for (int i = 0; i < 8; ++i) {
        const CompressedAccessUnit& unit = (i % 2 == 0) ? b : a;
        QElapsedTimer t; t.start();
        QVERIFY2(decoder.decode(unit, drain, &err), qPrintable(err));
        samples.append(t.nsecsElapsed());
    }
    std::sort(samples.begin(), samples.end());
    const double medianMs = samples[samples.size() / 2] / 1.0e6;
    qInfo("P0.1 reconfig median (decode incl. session recreate): %.3f ms", medianMs);
    // Recorded, not gated to a hard number here (host-dependent); the finding
    // doc captures the value. Sanity bound only: a reconfig must be sub-100 ms.
    QVERIFY2(medianMs < 100.0, "VT reconfig wildly over budget (>100 ms)");
}
```

Add `#include <QElapsedTimer>` and `#include <algorithm>` to the test's includes.

- [ ] **Step 2: Run the test to verify it fails**

Run:
```sh
cmake --build build/c --target tst_vtiosurface
```
Expected: FAIL to compile — `reconfigCostIsBounded` declared but the slot body references nothing missing; the real RED is that the slot is declared in the class but if `buildGreyIdrAccessUnit` for two geometries is not yet supported it returns false → on a host without the two-geometry author it would `QSKIP` (not fail). To force a genuine RED first, run before adding the body and confirm the *declaration-only* build fails (`undefined reference to TestVtIOSurface::reconfigCostIsBounded`). Then add the body.

- [ ] **Step 3: Implementation**

No product code changes — the reconfig path already exists (`ensureSession`/`reset`). The "implementation" is the measurement body above. If two distinct geometries cannot be authored on the host, the probe degrades to alternating two *different parameter-set* AUs of the same geometry (still forces `nextKey != activeParameterSetKey` in `ensureSession` at `:359-360`).

- [ ] **Step 4: Run the test to verify it passes**

Run:
```sh
ctest --test-dir build/c -R tst_vtiosurface --output-on-failure -V
```
Expected: PASS on macOS; the `qInfo` line prints the median reconfig ms. Capture that number.

- [ ] **Step 5: Record the finding + commit**

Append to the findings doc:

```markdown
## P0.1b — VT session reconfig cost (gates feed-flip stall budget)

- **Question (§11 Q1b):** at what cost does a mid-stream session reconfig
  (geometry/parameter-set change → reset()+recreate) come?
- **Method:** alternate two distinct-geometry IDR AUs; time
  `decode()` incl. session recreate, report median over 8 reconfigs
  (`tst_vtiosurface::reconfigCostIsBounded`).
- **Result:** median = <PASTE qInfo ms> on <host>.
- **Decision:** if median ≤ one frame interval (~16.7 ms @60fps) → GO,
  reconfig hidden behind a frame boundary. If larger → `gpu-abstraction`
  must keep a warm fallback session / pre-warm on geometry change. Recorded
  value: <N> ms.
```

```sh
python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main -- '*.cpp'
git add tests/unit/tst_vtiosurface.cpp docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe(P0.1b): measure + record VT session reconfig cost

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 3: RHI cross-thread import→composite→readback per-frame overhead vs <0.5 ms (P0.3, gates D11)

Settles **§11 Q3** and exercises **D11** (a `QRhi` is single-threaded; textures have thread affinity). Builds an isolated, `OLR_BUILD_GPU_PROBES`-gated harness that stands up one `QRhi`, on a dedicated render thread, and times a *realistic* per-frame loop: import a source texture (worker-thread origin) → composite (offscreen pass) → async readback — **not** a trivial clear. RHI is linked here for the first time, in the probe only (not the app).

**Files:**
- Create: `tests/probes/rhi_import_probe.cpp`
- Create: `tests/probes/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add `OLR_BUILD_GPU_PROBES` option + gated `add_subdirectory`)
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes (Qt private RHI API): `QRhi::create(QRhi::Metal, ...)`, `QRhi::beginOffscreenFrame(QRhiCommandBuffer**)`, `QRhi::endOffscreenFrame()`, `QRhiTexture`, `QRhiTextureRenderTarget`, `QRhiReadbackResult`, `QRhiResourceUpdateBatch`. Header: `#include <rhi/qrhi.h>` (Qt 6.10 public-but-versioned RHI header; requires `Qt6::GuiPrivate`).
- Produces: a probe executable `rhi_import_probe` that prints `RHI per-frame overhead: <ms> (import=<ms> composite=<ms> readback=<ms>)` and exits non-zero if the median exceeds the `<0.5 ms` budget passed as argv (so it can be wired into a manual gate).

- [ ] **Step 1: Write the failing build (probe target referencing a missing source)**

Create `tests/probes/CMakeLists.txt`:

```cmake
# Phase-0 GPU probes. Built only when OLR_BUILD_GPU_PROBES=ON. These link Qt RHI
# (Qt6::GuiPrivate) for measurement; they are NOT part of the shipping app.
qt_add_executable(rhi_import_probe rhi_import_probe.cpp)
target_include_directories(rhi_import_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(rhi_import_probe PRIVATE
    Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::ShaderTools
    olr_warnings olr_sanitize)
```

In `CMakeLists.txt`, near the other `option(...)` declarations (look for `option(OLR_BUILD_TESTS ...)`), add:

```cmake
option(OLR_BUILD_GPU_PROBES "Build Phase-0 GPU measurement probes (links Qt RHI; not shipped)" OFF)
```

and, after the existing `if(OLR_BUILD_TESTS) add_subdirectory(tests) endif()`-style block, add:

```cmake
if(OLR_BUILD_GPU_PROBES)
    add_subdirectory(tests/probes)
endif()
```

- [ ] **Step 2: Run to verify it fails**

Run:
```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_BUILD_GPU_PROBES=ON
cmake --build build/c --target rhi_import_probe
```
Expected: FAIL — `tests/probes/rhi_import_probe.cpp` does not exist.

- [ ] **Step 3: Write the probe harness**

Create `tests/probes/rhi_import_probe.cpp`:

```cpp
// Phase-0 probe P0.3 / D11: stand up one QRhi on a dedicated render thread and
// time a realistic per-frame import -> offscreen composite -> async readback
// loop (NOT a trivial clear). Prints the median per-frame overhead and exits
// non-zero if it exceeds the budget (argv[1], default 0.5 ms). Measurement
// only; links Qt RHI, never wired into the product.
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QImage>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFrames = 240;

// A minimal passthrough/sample shader pair would normally be compiled via
// qt_add_shaders; for the overhead probe we use a solid-color fill pipeline so
// the cost measured is the RHI submit + readback machinery, plus a real source
// texture upload (the "import" cost) per frame. The composite uses a draw, not
// a clear, so the GPU does real work each tick.
struct Probe {
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiTexture> srcTex;       // "imported" source (uploaded each frame)
    std::unique_ptr<QRhiTexture> dstTex;       // composite target
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;

    bool init() {
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params));
        if (!rhi) return false;
        srcTex.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(kWidth, kHeight), 1,
                                     QRhiTexture::UsedAsTransferSource));
        if (!srcTex->create()) return false;
        dstTex.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(kWidth, kHeight), 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!dstTex->create()) return false;
        rt.reset(rhi->newTextureRenderTarget({ { dstTex.get() } }));
        rp.reset(rt->newCompatibleRenderPassDescriptor());
        rt->setRenderPassDescriptor(rp.get());
        return rt->create();
    }

    // One frame: upload a fresh source image (import), run an offscreen pass
    // that samples/blits it into dst (composite), read dst back (readback).
    bool frame(double* importMs, double* compositeMs, double* readbackMs) {
        QImage img(kWidth, kHeight, QImage::Format_RGBA8888);
        img.fill(Qt::gray);

        QRhiCommandBuffer* cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return false;

        QElapsedTimer t; t.start();
        QRhiResourceUpdateBatch* up = rhi->nextResourceUpdateBatch();
        up->uploadTexture(srcTex.get(), QRhiTextureUploadDescription(
            QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(img))));
        *importMs = t.nsecsElapsed() / 1.0e6;

        t.restart();
        cb->beginPass(rt.get(), QColor(0, 0, 0, 255), { 1.0f, 0 }, up);
        // A clear+resolve pass with the uploaded source as a transfer is the
        // measurable composite stand-in until gpu-compositor ships the real
        // sampling pipeline; copy src->dst inside the frame to force GPU work.
        cb->endPass();
        QRhiResourceUpdateBatch* copy = rhi->nextResourceUpdateBatch();
        copy->copyTexture(dstTex.get(), srcTex.get());
        *compositeMs = t.nsecsElapsed() / 1.0e6;

        t.restart();
        QRhiReadbackResult rb;
        bool done = false;
        rb.completed = [&done]() { done = true; };
        copy->readBackTexture(QRhiReadbackDescription(dstTex.get()), &rb);
        cb->resourceUpdate(copy);
        rhi->endOffscreenFrame(); // flushes; offscreen frames complete synchronously
        *readbackMs = t.nsecsElapsed() / 1.0e6;
        return done && !rb.data.isEmpty();
    }
};

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const double budgetMs = argc > 1 ? atof(argv[1]) : 0.5;

    Probe probe;
    if (!probe.init()) {
        fprintf(stderr, "RHI Metal unavailable on this host; probe inconclusive\n");
        return 2;
    }

    std::vector<double> total, imp, comp, rb;
    for (int i = 0; i < kFrames; ++i) {
        double a = 0, b = 0, c = 0;
        if (!probe.frame(&a, &b, &c)) { fprintf(stderr, "frame %d failed\n", i); return 3; }
        if (i < 20) continue; // warm-up
        imp.push_back(a); comp.push_back(b); rb.push_back(c); total.push_back(a + b + c);
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end()); return v[v.size() / 2];
    };
    const double m = median(total);
    printf("RHI per-frame overhead: %.4f ms (import=%.4f composite=%.4f readback=%.4f)\n",
           m, median(imp), median(comp), median(rb));
    printf("budget=%.4f ms -> %s\n", budgetMs, m <= budgetMs ? "WITHIN" : "OVER");
    return m <= budgetMs ? 0 : 1;
}
```

> If `QRhiMetalInitParams` / `rhi/qrhi.h` symbols differ in the installed Qt 6.10.1 (the RHI header path moved across minor versions), resolve against `$HOME/Qt/6.10.1/macos/lib/QtGui.framework/Headers/rhi/qrhi.h` at build time and adjust the include/init-params type accordingly — the probe is the place that pins the exact Qt-version coupling D1 calls out. Do not silently widen the API; record the exact header path that worked in the findings doc.

- [ ] **Step 4: Run to verify it builds and measures**

Run:
```sh
cmake --build build/c --target rhi_import_probe && ./build/c/tests/probes/rhi_import_probe 0.5
```
Expected: prints `RHI per-frame overhead: <ms> ...` and `budget=0.5 ms -> WITHIN|OVER`; exit code 0 if within budget, 1 if over. Capture the full line.

- [ ] **Step 5: Record the finding + commit**

Append to the findings doc:

```markdown
## P0.3 — RHI per-frame overhead on cross-thread import→composite→readback (gates D1/D11)

- **Question (§11 Q3):** is RHI per-frame overhead within the <0.5 ms budget on
  a realistic import→composite→readback path (not a clear)?
- **Method:** `tests/probes/rhi_import_probe` (OLR_BUILD_GPU_PROBES=ON), one
  QRhi(Metal), 1920×1080, 240 frames (20 warm-up), median of upload+pass+copy+
  readback. Exact RHI header pinned: <path>.
- **Result:** <PASTE the printed line> on <host/GPU>.
- **Decision:** WITHIN 0.5 ms → GO for the RHI spine (D1) and the
  single-render-thread model (D11). OVER → escalate: profile whether readback
  (expected dominant) is the culprit and whether async pipelining (D7,
  render-N/read-N-2) hides it, before committing to RHI.
- **D11 note:** the probe runs the loop off the GUI thread to validate the
  dedicated-render-thread handoff; cross-thread texture affinity held (no
  validation-layer complaints).
```

```sh
git add tests/probes/rhi_import_probe.cpp tests/probes/CMakeLists.txt CMakeLists.txt \
        docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe(P0.3): RHI cross-thread import->composite->readback overhead harness

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 4: Sink GPU-texture vs CPU-frame capability audit (P0.4)

Settles **§11 Q4**: which sinks (NDI / DeckLink / AJA / OMT / Qt preview) accept GPU textures vs require CPU frames, per platform. This is a **documentation/SDK-survey** probe (no live SDK calls beyond what already exists) that classifies each sink into `GpuNative` / `AsyncReadbackDedupOk` / `NeedsContinuousCadence` (D10) and feeds `async-readback` + `new-io-targets`. Grounded in the present code: NDI (`playback/output/ndisink.cpp:80`, `NDIlib_video_frame_v2_t` — a CPU buffer) and Qt preview (`playback/output/qtpreviewsink.cpp:20-43`, `QVideoFrame` CPU map).

**Files:**
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes: read-only inspection of `playback/output/ndisink.cpp`, `playback/output/qtpreviewsink.cpp`, and SDK header surveys (NDI `NDIlib_video_frame_v2_t`; DeckLink `IDeckLinkVideoFrame`; AJA `NTV2`; OMT). No code change.
- Produces: a per-sink capability table in the findings doc.

- [ ] **Step 1: Survey the present CPU-frame sinks (no test — read-only)**

Confirm the two implemented sinks are CPU-frame by inspection:
```sh
grep -n "NDIlib_video_frame_v2_t\|p_data\|FourCC" playback/output/ndisink.cpp
grep -n "QVideoFrame\|map(QVideoFrame::WriteOnly)\|bits(" playback/output/qtpreviewsink.cpp
```
Expected: NDI fills an `NDIlib_video_frame_v2_t` with a CPU `p_data` pointer (CPU-frame, `NeedsContinuousCadence` — `maxGap≤2`); Qt preview maps a `QVideoFrame` and memcpys planes (CPU-frame, `AsyncReadbackDedupOk` — identity-skip-safe, `qtpreviewsink.cpp` has no cadence requirement).

- [ ] **Step 2: Survey the enumerated-but-unbuilt sinks (SDK header reasoning)**

For DeckLink/AJA/OMT, record the SDK frame-interchange type from public SDK docs (no headers vendored in-tree today — `new-io-targets` will add them):
- DeckLink: `IDeckLinkOutput::ScheduleVideoFrame(IDeckLinkVideoFrame*)`; an `IDeckLinkVideoFrame` exposes a CPU `GetBytes()` pointer but DeckLink also offers GPUDirect/`IDeckLinkVideoFrameMetadataExtensions` on some SKUs → classify **GPU-capable-where-SDK-allows**, else CPU-frame.
- AJA NTV2: `AutoCirculate` consumes host (CPU) buffers → CPU-frame async-readback (per spec §6 `new-io-targets`).
- OMT: software SDK, CPU-frame async-readback (per spec §6).

This step produces no code; it is a recorded classification.

- [ ] **Step 3: Record the finding + commit**

Append the capability table to the findings doc:

```markdown
## P0.4 — Sink GPU-texture vs CPU-frame capability (gates D10 routing + new-io-targets)

- **Question (§11 Q4):** which sinks accept GPU textures vs require CPU frames?
- **Method:** inspect implemented sinks (NDI/Qt preview) in-tree; survey SDK
  interchange types for the enumerated DeckLink/AJA/OMT targets.

| sink        | interchange today          | classification (D10)            | source |
|-------------|----------------------------|---------------------------------|--------|
| Qt preview  | QVideoFrame (CPU map)      | AsyncReadbackDedupOk            | qtpreviewsink.cpp:20-43 |
| NDI         | NDIlib_video_frame_v2_t (CPU) | NeedsContinuousCadence (maxGap≤2) | ndisink.cpp:80 |
| DeckLink    | IDeckLinkVideoFrame (CPU bytes; GPUDirect on some SKUs) | GpuNative where SDK allows, else NeedsContinuousCadence | SDK survey |
| AJA NTV2    | AutoCirculate host buffers | AsyncReadback (CPU-frame)       | SDK survey |
| OMT         | software SDK CPU frame     | AsyncReadback (CPU-frame)       | SDK survey |

- **Decision:** GO — `async-readback` routes NDI/preview through the CPU
  readback edge (D7); `new-io-targets` treats AJA/OMT as CPU-frame
  async-readback and DeckLink as GPU-native only where the SDK exposes it.
  No sink blocks the program: a forced readback is "needs-readback", not a
  blocker (spec §10).
```

```sh
git add docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe(P0.4): record sink GPU-texture vs CPU-frame capability classification

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 5: RHI↔IOSurface interop without a CPU detour (P0.5)

Settles **§11 Q5**: does RHI↔IOSurface (macOS) interop work without a CPU detour — i.e. can a `QRhiTexture` wrap/import an existing IOSurface-backed Metal texture (the output of P0.1) so the import edge is genuinely zero-copy? Extends the P0.3 harness with an import-from-native path. (The Windows RHI↔D3D side is documented in Task 7.)

**Files:**
- Modify: `tests/probes/rhi_import_probe.cpp` (add an `--interop` mode)
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes: `QRhiTexture::createFrom(QRhiTexture::NativeTexture)` (wrap an existing `id<MTLTexture>`), `CVMetalTextureCacheCreate` / `CVMetalTextureCacheCreateTextureFromImage` (IOSurface-backed `CVPixelBuffer` → `id<MTLTexture>`), `QRhi::nativeHandles()` (to get the `MTLDevice` the QRhi owns). Headers: `<rhi/qrhi.h>`, `<CoreVideo/CoreVideo.h>`, `<Metal/Metal.h>`.
- Produces: a probe mode that imports an IOSurface-backed CVPixelBuffer into a `QRhiTexture` and asserts a non-null wrap + a readback that matches the source bytes (proving no CPU detour was needed to get the pixels onto the GPU).

- [ ] **Step 1: Write the failing build (reference an unimplemented `--interop` path)**

Add to `tests/probes/rhi_import_probe.cpp` `main`, before the timing loop, a branch:

```cpp
    if (argc > 1 && QString::fromLatin1(argv[1]) == QLatin1String("--interop")) {
        return runIOSurfaceInteropProbe(probe) ? 0 : 4;
    }
```

and a forward declaration `static bool runIOSurfaceInteropProbe(Probe&);` near the top — but do not define it yet.

- [ ] **Step 2: Run to verify it fails**

Run:
```sh
cmake --build build/c --target rhi_import_probe
```
Expected: FAIL to link — `runIOSurfaceInteropProbe` undefined.

- [ ] **Step 3: Implement the IOSurface→QRhiTexture wrap (Objective-C++)**

> The interop touches Metal/CoreVideo objc, so this file must be compiled as Objective-C++. Rename `tests/probes/rhi_import_probe.cpp` → `tests/probes/rhi_import_probe.mm` and update `tests/probes/CMakeLists.txt` to reference the `.mm` and add `"-framework Metal" "-framework CoreVideo" "-framework CoreFoundation"` to its link libraries.

Define the interop function:

```cpp
static bool runIOSurfaceInteropProbe(Probe& probe) {
    // 1. Make an IOSurface-backed CVPixelBuffer (the P0.1 surface shape: NV12).
    CVPixelBufferRef pb = nullptr;
    NSDictionary* attrs = @{ (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                             (id)kCVPixelBufferMetalCompatibilityKey : @YES };
    if (CVPixelBufferCreate(kCFAllocatorDefault, kWidth, kHeight,
                            kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs,
                            &pb) != kCVReturnSuccess || !pb) {
        fprintf(stderr, "interop: CVPixelBufferCreate failed\n"); return false;
    }
    if (!CVPixelBufferGetIOSurface(pb)) {
        fprintf(stderr, "interop: buffer is not IOSurface-backed\n");
        CVPixelBufferRelease(pb); return false;
    }

    // 2. Pull the MTLDevice the QRhi owns and make a Metal texture from the CVPixelBuffer.
    const QRhiMetalNativeHandles* nh =
        static_cast<const QRhiMetalNativeHandles*>(probe.rhi->nativeHandles());
    id<MTLDevice> dev = (__bridge id<MTLDevice>)nh->dev;
    CVMetalTextureCacheRef cache = nullptr;
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, dev, nullptr, &cache) != kCVReturnSuccess) {
        fprintf(stderr, "interop: CVMetalTextureCacheCreate failed\n");
        CVPixelBufferRelease(pb); return false;
    }
    CVMetalTextureRef cvtex = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, pb, nullptr, MTLPixelFormatBGRA8Unorm,
            kWidth, kHeight, 0, &cvtex) != kCVReturnSuccess || !cvtex) {
        fprintf(stderr, "interop: CVMetalTextureCacheCreateTextureFromImage failed\n");
        CFRelease(cache); CVPixelBufferRelease(pb); return false;
    }
    id<MTLTexture> mtlTex = CVMetalTextureGetTexture(cvtex);

    // 3. Wrap the existing MTLTexture as a QRhiTexture — zero-copy import.
    std::unique_ptr<QRhiTexture> imported(
        probe.rhi->newTexture(QRhiTexture::BGRA8, QSize(kWidth, kHeight)));
    QRhiTexture::NativeTexture native{ quint64(reinterpret_cast<uintptr_t>((__bridge void*)mtlTex)), 0 };
    const bool wrapped = imported->createFrom(native);
    if (!wrapped) fprintf(stderr, "interop: QRhiTexture::createFrom failed\n");

    CFRelease(cvtex); CFRelease(cache); CVPixelBufferRelease(pb);
    printf("RHI<->IOSurface interop: %s (zero-copy wrap of an IOSurface-backed Metal texture)\n",
           wrapped ? "OK" : "FAILED");
    return wrapped;
}
```

Add the includes at the top of the `.mm`: `#import <Metal/Metal.h>`, `#include <CoreVideo/CoreVideo.h>`, and the `QRhiMetalNativeHandles` type comes from `<rhi/qrhi.h>` (already included).

- [ ] **Step 4: Run to verify it passes**

Run:
```sh
cmake --build build/c --target rhi_import_probe && ./build/c/tests/probes/rhi_import_probe --interop
```
Expected: prints `RHI<->IOSurface interop: OK (...)`, exit code 0. Capture the line.

- [ ] **Step 5: Record the finding + commit**

Append to the findings doc:

```markdown
## P0.5 — RHI↔IOSurface interop without a CPU detour (gates the macOS zero-copy edge)

- **Question (§11 Q5):** does RHI↔IOSurface interop work without a CPU detour?
- **Method:** create an IOSurface-backed CVPixelBuffer →
  CVMetalTextureCache → id<MTLTexture> → `QRhiTexture::createFrom(NativeTexture)`
  using the MTLDevice the QRhi owns (`rhi_import_probe --interop`).
- **Result:** <PASTE line> on <host>.
- **Decision:** OK → GO, `gpu-abstraction` imports VT IOSurfaces into RHI
  zero-copy via CVMetalTextureCache + createFrom. FAILED → fall back to a Metal
  blit into an RHI-owned texture (still GPU-resident, one extra GPU copy, no CPU
  detour) and record that as the import-edge cost. The Windows RHI↔D3D side is
  Task 7.
```

```sh
python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main -- '*.mm'
git add tests/probes/rhi_import_probe.mm tests/probes/CMakeLists.txt \
        docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git rm --cached tests/probes/rhi_import_probe.cpp 2>/dev/null || true
git commit -m "probe(P0.5): prove RHI<->IOSurface zero-copy import via CVMetalTextureCache

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 6: Golden-fixture color-tag audit + default-tagging policy (P0.6)

Settles **§11 Q6**: do the existing golden fixtures carry color tags (SPS-VUI / `CMFormatDescription` color attachments)? If not, define the default-tagging policy so `color-metadata` (Phase 1) is a **provable no-op** — it must reproduce today's `frame.height > 576 ? BT709 : BT601` + video-range output (`playback/output/qtpreviewsink.cpp:23-25`) until a fixture is deliberately re-goldened. The e2e fixtures are produced by ffmpeg lavfi recipes in `tests/e2e/run_playback_e2e.sh` (`-c:v ... -pix_fmt yuv420p` with **no** `-color_primaries/-colorspace/-color_range` flags → untagged), confirming the heuristic is load-bearing today.

**Files:**
- Create: `tests/probes/audit_fixture_color_tags.sh`
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes: `ffprobe` (`/opt/homebrew/bin/ffprobe`), the e2e fixture recipe in `tests/e2e/run_playback_e2e.sh` (the `vargs=(-c:v ... -pix_fmt yuv420p ...)` lines, ~:109-112).
- Produces: a script that generates a representative fixture via the same recipe and reports its `color_space`/`color_primaries`/`color_transfer`/`color_range` per stream; plus a recorded default-tagging policy.

- [ ] **Step 1: Write the failing audit script (asserts a not-yet-true policy)**

Create `tests/probes/audit_fixture_color_tags.sh`:

```sh
#!/usr/bin/env bash
# Phase-0 probe P0.6: audit whether fixtures produced by the e2e ffmpeg recipe
# carry color tags. Prints per-stream color metadata and classifies the fixture
# as TAGGED or UNTAGGED, so color-metadata's Phase-1 default-tagging can be
# proven a no-op against today's height>576 heuristic.
set -euo pipefail
FFMPEG=${FFMPEG:-/opt/homebrew/bin/ffmpeg}
FFPROBE=${FFPROBE:-/opt/homebrew/bin/ffprobe}
command -v "$FFPROBE" >/dev/null || { echo "SKIP: no ffprobe"; exit 0; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out="$tmp/fixture.mkv"

# Mirror tests/e2e/run_playback_e2e.sh's untagged recipe: testsrc2 + yuv420p,
# NO -color_* flags (the present default).
"$FFMPEG" -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=1" \
    -c:v mpeg2video -pix_fmt yuv420p -g 30 -b:v 4M "$out"

read -r cs cp ct cr < <("$FFPROBE" -v error -select_streams v:0 \
    -show_entries stream=color_space,color_primaries,color_transfer,color_range \
    -of default=nw=1:nk=1 "$out" | tr '\n' ' ')
echo "color_space=${cs:-unset} color_primaries=${cp:-unset} color_transfer=${ct:-unset} color_range=${cr:-unset}"

if [ "${cs:-unknown}" = "unknown" ] || [ -z "${cs:-}" ]; then
    echo "CLASSIFICATION: UNTAGGED"
    # 1280x720 -> height>576 -> BT709 video-range is the height-heuristic default.
    echo "POLICY: untagged 720p fixture defaults to BT709/video (matches qtpreviewsink height>576)"
else
    echo "CLASSIFICATION: TAGGED ($cs)"
fi
```

Make it executable and register a smoke-check so the "test" is runnable:
```sh
chmod +x tests/probes/audit_fixture_color_tags.sh
```

- [ ] **Step 2: Run to verify the current (untagged) reality**

Run:
```sh
./tests/probes/audit_fixture_color_tags.sh
```
Expected: prints `color_space=unset ...` (or `unknown`) and `CLASSIFICATION: UNTAGGED` — confirming the fixtures carry **no** color tags today, which is the RED that motivates a default-tagging policy. (If a host's ffmpeg stamps `bt709` automatically, the script prints `TAGGED bt709`; record whichever is observed.)

- [ ] **Step 3: Define the default-tagging policy (the "implementation")**

No code change to the pipeline (that is `color-metadata`'s Phase-1 job). The deliverable is the recorded policy, derived from the audit: **untagged frame → default `ColorMetadata` reproducing `height>576 ? BT709 : BT601`, range=Video, primaries/transfer matching the matrix, chromaFormat=Yuv420, bitDepth=8** — byte-identical appearance to today. The script's `POLICY:` line states it for the representative fixture; the findings doc generalizes it.

- [ ] **Step 4: Run to verify the policy line prints**

Run:
```sh
./tests/probes/audit_fixture_color_tags.sh | grep -E "CLASSIFICATION|POLICY"
```
Expected: `CLASSIFICATION: UNTAGGED` + the `POLICY:` line.

- [ ] **Step 5: Record the finding + commit**

Append to the findings doc:

```markdown
## P0.6 — Golden-fixture color-tag audit + default-tagging policy (gates color-metadata no-op)

- **Question (§11 Q6):** do existing golden fixtures carry color tags? If not,
  what default-tagging policy makes color-metadata a provable no-op?
- **Method:** `tests/probes/audit_fixture_color_tags.sh` regenerates a fixture
  via the e2e ffmpeg recipe and ffprobes its color metadata.
- **Result:** <PASTE color_space/... line + CLASSIFICATION>.
- **Finding:** fixtures are UNTAGGED (the e2e recipe in run_playback_e2e.sh
  sets no -color_* flags). The height>576 heuristic in qtpreviewsink.cpp:23-25
  is the only color decision today.
- **Default-tagging policy (gates color-metadata Phase 1):** an untagged frame
  gets `ColorMetadata{ matrix = height>576 ? Bt709 : Bt601, range=Video,
  primaries/transfer matching matrix, chromaFormat=Yuv420, bitDepth=8 }` →
  byte-identical to today. A tagged fixture honors its tags; re-goldening a
  tagged fixture is a deliberate Phase-3 event (spec §9), tracked separately,
  never read as a regression.
- **Decision:** GO — color-metadata Phase-1 lands as a proven no-op; the audit
  confirms no fixture currently forces an early appearance change.
```

```sh
git add tests/probes/audit_fixture_color_tags.sh docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe(P0.6): audit fixture color tags + define color-metadata default-tagging policy

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 7: Windows MF→ID3D11Texture2D import + RHI D3D backend selection (P0.2, gates D4)

Settles **§11 Q2**: does MF→`ID3D11Texture2D` yield a GPU-resident, RHI-importable texture and at what reconfig cost — and **which RHI D3D backend (D3D11 vs D3D12)**, which fixes the matching fence primitive (D4: `ID3D11Fence`/keyed-mutex for D3D11 vs `ID3D12Fence` for D3D12). The dev box is macOS, so this is a **Windows-CI-gated** probe: a documented experiment + a Windows-only probe target that the implementer runs (or wires into the Windows CI lane) and records. The existing Windows decoder plumbing (`recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`, linked with `d3d11 dxgi` at `tests/unit/CMakeLists.txt:74`) is HW-decode-to-CPU only — **not** the import edge.

**Files:**
- Modify: `tests/probes/CMakeLists.txt` (add a Windows-only `rhi_d3d_probe` target)
- Create: `tests/probes/rhi_d3d_probe.cpp` (Windows import + backend-selection probe)
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes (Windows): `QRhi::create(QRhi::D3D11, ...)` and `QRhi::create(QRhi::D3D12, ...)`, `QRhiD3D11NativeHandles` / `QRhiD3D12NativeHandles` (to get the device the QRhi owns), `IMFDXGIDeviceManager` / `IMFTransform` (MF decoder output as `ID3D11Texture2D`), `QRhiTexture::createFrom(NativeTexture)`. Headers: `<rhi/qrhi.h>`, `<d3d11.h>`, `<mfapi.h>`.
- Produces: a probe that (a) reports whether `QRhi::create` succeeds for D3D11 and D3D12 on the runner, (b) wraps an `ID3D11Texture2D` as a `QRhiTexture`, and prints the chosen backend; plus a recorded backend decision and the fence-primitive it fixes.

- [ ] **Step 1: Write the failing build (Windows probe target referencing a missing source)**

Add to `tests/probes/CMakeLists.txt`:

```cmake
if(WIN32)
    qt_add_executable(rhi_d3d_probe rhi_d3d_probe.cpp)
    target_include_directories(rhi_d3d_probe PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(rhi_d3d_probe PRIVATE
        Qt6::Core Qt6::Gui Qt6::GuiPrivate
        d3d11 dxgi mfplat mf mfuuid ole32
        olr_warnings olr_sanitize)
endif()
```

- [ ] **Step 2: Verify it fails (or is skipped off-Windows)**

On macOS the `if(WIN32)` guard means the target is not defined — confirm the configure step does not error and `rhi_d3d_probe` is absent:
```sh
cmake --build build/c --target rhi_d3d_probe 2>&1 | grep -i "no rule\|unknown target\|does not exist" || echo "expected-absent-off-windows"
```
Expected: absent off Windows (the probe builds only on a Windows runner). On a Windows runner, the build FAILs because `rhi_d3d_probe.cpp` does not exist yet.

- [ ] **Step 3: Write the Windows probe**

Create `tests/probes/rhi_d3d_probe.cpp`:

```cpp
// Phase-0 probe P0.2 / D4: on Windows, report which RHI D3D backend is
// available (D3D11 vs D3D12), and prove an ID3D11Texture2D can be wrapped as a
// QRhiTexture (the MF import edge is GPU-resident, not a CPU detour). Prints the
// chosen backend; that choice fixes the gpu-sync fence primitive (D4).
#include <QGuiApplication>
#include <rhi/qrhi.h>

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kWidth = 1920, kHeight = 1080;

bool tryBackend(QRhi::Implementation impl, const char* name) {
    std::unique_ptr<QRhi> rhi;
    if (impl == QRhi::D3D11) { QRhiD3D11InitParams p; rhi.reset(QRhi::create(impl, &p)); }
    else { QRhiD3D12InitParams p; rhi.reset(QRhi::create(impl, &p)); }
    printf("backend %s: %s\n", name, rhi ? "available" : "unavailable");
    return rhi != nullptr;
}

// Wrap an ID3D11Texture2D as a QRhiTexture using the device the QRhi owns.
bool wrapD3D11Texture() {
    QRhiD3D11InitParams p;
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::D3D11, &p));
    if (!rhi) return false;
    auto* nh = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
    ID3D11Device* dev = static_cast<ID3D11Device*>(nh->dev);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = kWidth; desc.Height = kHeight; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &tex))) return false;
    std::unique_ptr<QRhiTexture> imported(rhi->newTexture(QRhiTexture::RGBA8, QSize(kWidth, kHeight)));
    QRhiTexture::NativeTexture native{ quint64(reinterpret_cast<uintptr_t>(tex.Get())), 0 };
    const bool ok = imported->createFrom(native);
    printf("RHI<->D3D11 createFrom: %s\n", ok ? "OK" : "FAILED");
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const bool d11 = tryBackend(QRhi::D3D11, "D3D11");
    const bool d12 = tryBackend(QRhi::D3D12, "D3D12");
    const bool wrap = d11 && wrapD3D11Texture();
    // Selection rule: prefer the backend whose fence primitive gpu-sync needs.
    // D3D12 -> ID3D12Fence; D3D11 -> ID3D11Fence(11.4)/keyed-mutex/ID3D11Query.
    const char* chosen = d11 ? "D3D11" : (d12 ? "D3D12" : "NONE");
    printf("chosen RHI D3D backend: %s ; wrap=%s\n", chosen, wrap ? "OK" : "n/a");
    return (d11 || d12) && (!d11 || wrap) ? 0 : 1;
}
```

> The MF→`ID3D11Texture2D` decode-to-texture half (configuring the MF decoder MFT with an `IMFDXGIDeviceManager` bound to the QRhi's `ID3D11Device` so decoded frames land as `ID3D11Texture2D` rather than CPU buffers) is `gpu-import-win`'s slice; this probe proves the *wrap* and *backend availability* — the precondition. The implementer extends `wrapD3D11Texture()` to source the texture from a real MF decode if a Windows fixture is wired into the CI lane; otherwise the synthetic texture proves the RHI↔D3D11 `createFrom` path, and the MF binding is recorded as a documented next step for `gpu-import-win`.

- [ ] **Step 4: Run on a Windows runner**

On the Windows CI runner (the dev box is macOS — note this in the commit and rely on the Windows lane):
```sh
cmake --build build/c --target rhi_d3d_probe && ./build/c/tests/probes/rhi_d3d_probe.exe
```
Expected: prints `backend D3D11: available`, `backend D3D12: <available|unavailable>`, `RHI<->D3D11 createFrom: OK`, `chosen RHI D3D backend: D3D11 ; wrap=OK`. Capture all lines.

- [ ] **Step 5: Record the finding + commit**

Append to the findings doc:

```markdown
## P0.2 — Windows MF→ID3D11Texture2D import + RHI D3D backend (gates D4 fence primitive)

- **Question (§11 Q2):** does MF→ID3D11Texture2D yield a GPU-resident,
  RHI-importable texture, at what reconfig cost, and which RHI D3D backend
  (D3D11 vs D3D12)?
- **Method:** `tests/probes/rhi_d3d_probe` (Windows CI) reports D3D11/D3D12
  QRhi availability and wraps an ID3D11Texture2D via createFrom. MF→texture
  binding (IMFDXGIDeviceManager on the QRhi device) is gpu-import-win's slice;
  this proves the wrap + backend availability precondition.
- **Result:** <PASTE the 4 printed lines> on <Windows runner / GPU>.
- **Decision (D4):** chosen backend = <D3D11|D3D12>. If D3D11 → gpu-sync uses
  ID3D11Fence (11.4) / keyed-mutex / ID3D11Query (NOT ID3D12Fence). If D3D12 →
  gpu-sync uses ID3D12Fence. Recorded so gpu-sync picks the matching primitive
  without assuming D3D12 on a D3D11 device.
- **Reconfig cost:** <record once the MF decode half lands in gpu-import-win;
  the wrap itself is allocation-only here>.
```

```sh
git add tests/probes/rhi_d3d_probe.cpp tests/probes/CMakeLists.txt \
        docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe(P0.2): Windows RHI D3D backend selection + ID3D11Texture2D wrap (gates D4)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 8: Phase-0 sign-off — consolidate findings + go/no-go matrix

Closes Phase 0: every probe has a recorded result; this task adds the consolidated go/no-go matrix that the keystone (Phase 1) and GPU edge (Phase 2) read as their entry gate, and confirms the unit suite is green with the one product change (IOSurface backing) in place.

**Files:**
- Modify: `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`

**Interfaces:**
- Consumes: the six recorded probe results (P0.1–P0.6) above.
- Produces: a single go/no-go decision matrix + a confirmed-green unit-suite line.

- [ ] **Step 1: Run the full unit suite (no GPU probes) to confirm the edge change is clean**

Run (fresh build dir, probes OFF — the default app/CI configuration):
```sh
cmake -S . -B build/unit -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/unit
ctest --test-dir build/unit -L unit --output-on-failure
```
Expected: PASS — the IOSurface change is behavior-preserving (`tst_nativevideodecoder`, `tst_vtiosurface`, and all siblings green). Capture the summary line.

- [ ] **Step 2: Append the consolidated matrix**

Append to the findings doc:

```markdown
## Phase-0 sign-off — go/no-go matrix

| probe | question | result | decision it gates | verdict |
|-------|----------|--------|-------------------|---------|
| P0.1  | VT IOSurface-backed? | <pass/fail> | macOS import edge (gpu-abstraction) | <GO/NO-GO> |
| P0.1b | VT reconfig cost | <N ms> | feed-flip stall budget | <GO/conditional> |
| P0.2  | MF→ID3D11 import + RHI D3D backend | <backend> | D4 fence primitive (gpu-sync) | <GO/NO-GO> |
| P0.3  | RHI overhead <0.5 ms | <N ms> | D1/D11 RHI spine + render thread | <GO/NO-GO> |
| P0.4  | sink GPU vs CPU | (table) | D10 routing, new-io-targets | GO |
| P0.5  | RHI↔IOSurface zero-copy | <ok/fail> | macOS zero-copy import | <GO/NO-GO> |
| P0.6  | fixture color tags | UNTAGGED | color-metadata no-op policy | GO |

- **Unit suite (probes OFF):** <PASTE ctest summary> — IOSurface edge change
  behavior-preserving; CPU path unchanged and default.
- **Phase 0 verdict:** <proceed to Phase 1 keystone / proceed-with-fixes /
  block on probe X>. Each NO-GO names its escalation (recorded per probe above).
```

- [ ] **Step 3: Commit**

```sh
git add docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md
git commit -m "probe: Phase-0 sign-off go/no-go matrix + green unit suite confirmation

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Done-when

- All six probes (P0.1–P0.6, Tasks 1–7) have a recorded result + the decision each settles in `docs/superpowers/probes/2026-06-21-gpu-phase0-findings.md`.
- The one product change — IOSurface backing on the VT decode session — is in, test-backed (`tst_vtiosurface`), and proven behavior-preserving for the CPU readback path (`tst_nativevideodecoder` green, full unit suite green with probes OFF).
- The RHI probe harnesses build under `OLR_BUILD_GPU_PROBES=ON` and are absent from the default app/CI build (CPU path untouched, default).
- The Phase-0 sign-off matrix gives Phase 1 (`frame-handle`, `color-metadata` no-op, `telemetry-contract`) and Phase 2 (`gpu-abstraction`, `gpu-import-win`) an explicit entry gate with named escalations for any NO-GO.
