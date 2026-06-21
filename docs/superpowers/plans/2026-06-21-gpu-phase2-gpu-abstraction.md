# GPU Abstraction + macOS Vertical Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Bring up the macOS GPU edge of the pipeline — a platform-neutral `GpuSurface` wrapper over IOSurface/`CVPixelBuffer`, one `QRhi` on a dedicated render thread (D11), a VideoToolbox keep-surface import producing a GPU-backed `FrameHandle`, and a lazy `readToCpu` download — then ship the permanent single-feed vertical slice (§8) behind `OLR_GPU_PIPELINE` (default off): decode → GPU `FrameHandle` → existing single-source render → Qt preview via one lazy readback, with an early micro-stress (evict-while-render + OOM-degrade) guarded by a minimal decode-done fence.

**Architecture:** A new `playback/gpu/` family defines the concrete `GpuSurface` (the type Phase-1's `framehandle.h` forward-declared), a `GpuRhiContext` owning one `QRhi` (Metal) on a dedicated `GpuRenderThread`, a `GpuFrameData : IFrameData` that retains a `CVPixelBuffer` and downloads lazily through `QRhi`, a `DecodeDoneFence` (the first `gpu-sync` primitive, `MTLSharedEvent`), and a `VtKeepSurfaceImporter` that wraps the VideoToolbox decode output. The macOS slice branches the playback worker's H.264/keep-surface decode path on `OLR_GPU_PIPELINE` to emit GPU-backed handles that flow through the unchanged single-source render path and reach `FrameProvider` via exactly one `readToCpu`. The shared `gpusurface.h` header stays platform-neutral — no `CVPixelBuffer`/Metal types leak; the Apple types live only in `.mm` translation units.

**Tech Stack:** C++17, Qt 6 (Core/Gui/GuiPrivate for `QRhi`/`QRhiMetal`, Test), FFmpeg (`libav*` `AVFrame` at the CPU-origin edge), Apple VideoToolbox/CoreVideo/CoreMedia/Metal/IOSurface, CMake + Ninja. Consumes the Phase-1 `frame-handle` keystone (`playback/output/framehandle.*`), `format-canon`, `shader-toolchain`, and `telemetry-contract` contracts.

## Global Constraints

- **Keystone-first.** This subproject defines `GpuSurface` (the concrete behind Phase-1's opaque forward decl) and the `IFrameData` GPU concrete. Name every type/signature exactly as written; `gpu-import-win`, `gpu-sync`, `gpu-compositor`, `async-readback`, and `gpu-budget` consume these verbatim. Consume the frame-handle contract names exactly: `FrameHandle`, `IFrameData`, `CpuPlanes`, `FrameMetadata`, `FramePayloadKey`, `FramePixelFormat`, `ColorMetadata`, `makeCpuFrameHandle`, `solidYuv420pHandle`, `MediaVideoFrameView`. Do not invent variants.
- **CPU path stays default + reference.** `OLR_GPU_PIPELINE` is **off by default**. With it off, the pipeline is byte-for-byte the Phase-1 CPU path; the CPU pipeline remains the permanent correctness oracle and runtime fallback. A GPU-backed handle's `readToCpu()` must match the CPU-path decode of the same frame within **±1 LSB/channel**.
- **Everything behind flags.** Every GPU code path is reached only when `OLR_GPU_PIPELINE=1` *and* a `GpuRhiContext` constructed successfully. Any failure (RHI unavailable, surface alloc failure, GPU-OOM) degrades to the CPU handle without crashing the decode loop — never a hard dependency.
- **No throwaways.** Every artifact is production and stays in the tree. The Phase-0 probe code (IOSurface-backed decode session request) is the first piece of the real import edge, not a prototype.
- **Public-repo professionalism.** This repo is public. Code, comments, and commit messages must be self-contained and professional: document the present design, no internal notes, no references to private history.
- **Format changed lines only.** CI's Lint job checks clang-format on changed lines only; several engine files use hand-written Allman style. Format only the lines you change:
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'
  ```
- **The zero-regression gate (`OLR_GPU_PIPELINE=0`).** After every task, the entire existing unit suite (`ctest -L unit`) and `e2e_play` suite must pass with **identical assertion values and golden outputs**. The GPU path is purely additive; with the flag off nothing branches on GPU residency.
- **Build (run from the worktree root):** configure once:
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Unit tests register via `olr_add_unit_test(<name> olr_test_playback)` in `tests/unit/CMakeLists.txt`; Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`. GPU behavioral tests are gated on a GPU-capable host — where absent (`offscreen`/CI), they assert graceful null/degrade, never a hard fail.

---

## Preconditions (Phase-0 probe gates — read before Task 1)

This subproject is **heavily probe-contingent**. Phase 0 (§7) must have resolved the following. Each task below names which precondition it depends on; if a probe came back negative, follow the branch note.

- **P0.1 — VideoToolbox yields an IOSurface-backed, RHI-importable `CVPixelBuffer`.** Today the decode session (`recorder_engine/ingest/nativevideodecoder_videotoolbox.mm:324-356`, `createSession`) requests pixel format `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` but **does not** set `kCVPixelBufferIOSurfacePropertiesKey`, so the output is not guaranteed IOSurface-backed. **Task 4 adds that request** — it is the first real piece of the import edge. **If P0.1 fails** (VT cannot produce an IOSurface-backed buffer at acceptable reconfig cost): the slice cannot keep the surface GPU-resident. Branch: `GpuFrameData` is still built and unit-tested against a *manually-allocated* IOSurface-backed `CVPixelBuffer` (Task 3), but the slice's VT path (Task 6) wraps the CPU `AVFrame` in a CPU handle and the import edge is deferred to a follow-up; the `OLR_GPU_PIPELINE=1` picture-correctness gate then runs against an upload-from-CPU surface, not a kept one, and `copy-detector` is documented as not-yet-1 for the import edge.
- **P0.3 — RHI per-frame overhead is within the <0.5 ms budget on a realistic cross-thread import → composite → readback path.** Probe must exercise the threading model (a `QRhi` is single-threaded with texture thread-affinity, D11), not a clear pass. **If P0.3 fails** (overhead exceeds budget, or RHI↔IOSurface interop needs a CPU detour): the slice still ships behind the flag for correctness, but the performance criterion is recorded as not-met and `async-readback` (Phase 4) inherits the overhead-reduction work. The plan does not gate on hitting <0.5 ms — it gates on correctness + degrade safety.
- **P0.5 — RHI↔IOSurface interop works.** `QRhiTexture::createFrom`/native-texture import of an IOSurface-backed Metal texture (via `CVMetalTextureCache`) round-trips through `QRhi::beginOffscreenFrame`/`endOffscreenFrame` and a readback. **If P0.5 fails** (no zero-copy import): `GpuFrameData::readToCpu` falls back to a `CVPixelBufferLockBaseAddress` CPU download (the `copyPixelBufferToAvFrame` path already in `nativevideodecoder_videotoolbox.mm:129-191`) instead of an RHI readback; the handle is still "GPU-backed" (retains the surface) and the ±1-LSB gate uses the lock-download. RHI is then only on the render path, not the readback — documented, still correct.

**RHI availability (verified in this worktree):** the public RHI header is `$HOME/Qt/6.10.1/macos/lib/QtGui.framework/.../QtGui/rhi/qrhi.h` and the Metal backend private header is `.../QtGui/private/qrhimetal_p.h`, so `QRhi` + `QRhiMetalInitParams` are reachable via the `Qt6::GuiPrivate` module. Tasks that touch RHI link `Qt6::GuiPrivate`.

---

## File Structure

- **Create** `playback/gpu/gpusurface.h` — platform-neutral `GpuSurface` (the concrete behind the Phase-1 forward decl) + `GpuSurfaceDesc`. No Apple/Metal/`CVPixelBuffer` types in this header.
- **Create** `playback/gpu/gpusurface_apple.mm` — the Apple `GpuSurface` body that owns a `CVPixelBufferRef` (IOSurface-backed); all CoreVideo types confined here.
- **Create** `playback/gpu/gpurhicontext.h`, `playback/gpu/gpurhicontext_apple.mm` — `GpuRhiContext` owning one `QRhi` (Metal) on a dedicated `GpuRenderThread`; `beginOffscreenFrame`/`endOffscreenFrame` import→readback (D11).
- **Create** `playback/gpu/decodedonefence.h`, `playback/gpu/decodedonefence_apple.mm` — `DecodeDoneFence` (`MTLSharedEvent`), the first `gpu-sync` primitive.
- **Create** `playback/gpu/gpuframedata.h`, `playback/gpu/gpuframedata.cpp` — `GpuFrameData : IFrameData` retaining a `GpuSurface`, lazy `readToCpu` via the RHI context (or lock-download fallback per P0.5).
- **Create** `playback/gpu/vtkeepsurfaceimporter.h`, `playback/gpu/vtkeepsurfaceimporter_apple.mm` — wraps a VT decode output `CVImageBufferRef` into a GPU-backed `FrameHandle`.
- **Create** `playback/gpu/gpupipelineconfig.h` — `gpuPipelineEnabled()` reading `OLR_GPU_PIPELINE`, + the forced-tiny-budget / inject-OOM micro-stress hooks.
- **Modify** `recorder_engine/ingest/nativevideodecoder.h` / `nativevideodecoder_videotoolbox.mm` — add an opt-in `keepSurface` mode (request `kCVPixelBufferIOSurfacePropertiesKey`; deliver the `CVImageBufferRef` instead of/in addition to the CPU `AVFrame`). P0.1.
- **Modify** `playback/playbackworker.cpp` / `.h` — `OLR_GPU_PIPELINE` branch in `decodePacketIntoBank` to build GPU-backed handles; new `gpuReadToCpuCount` counter (telemetry-contract copy-detector).
- **Modify** `playback/frameprovider.cpp` / `.h` — accept a `FrameHandle` delivery that does the single `readToCpu` (screenshot CPU-`QImage` path + GUI-thread affinity).
- **Modify** `CMakeLists.txt` — compile `playback/gpu/*` per platform (Apple `.mm`, stub elsewhere); link `Qt6::GuiPrivate`, Metal/IOSurface frameworks; add a `playback/gpu/gpusurface_stub.cpp` for non-Apple.
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_gpusurface`, `tst_gpurhicontext`, `tst_gpuframedata`, `tst_decodedonefence`, `tst_gpu_microstress` against `olr_test_playback` (+ an Apple-only backend lib mirroring `olr_test_nativevideoencoder`).
- **Modify** `tests/e2e/play_harness.cpp`, `tests/e2e/run_playback_e2e.sh` — emit/assert `gpuReadToCpuCount`, run `play1x` under both `OLR_GPU_PIPELINE=0` and `=1`.
- **Create** tests: `tests/unit/tst_gpusurface.cpp`, `tst_gpurhicontext.cpp`, `tst_gpuframedata.cpp`, `tst_decodedonefence.cpp`, `tst_gpu_microstress.cpp`.

---

## Task 1: Platform-neutral `GpuSurface` + non-Apple stub (the concrete behind the forward decl)

**Precondition:** none (pure interface; defines the type Phase-1 forward-declared).

**Files:**
- Create: `playback/gpu/gpusurface.h`, `playback/gpu/gpusurface_apple.mm`, `playback/gpu/gpusurface_stub.cpp`
- Test: `tests/unit/tst_gpusurface.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `FramePixelFormat` (`playback/output/framepixelformat.h`), the opaque `class GpuSurface;` forward decl in `playback/output/framehandle.h:662`.
- Produces:
  ```cpp
  // playback/gpu/gpusurface.h — NO Apple/Metal/CVPixelBuffer types here.
  struct GpuSurfaceDesc {
      FramePixelFormat format = FramePixelFormat::Nv12;
      int width = 0;
      int height = 0;
  };
  class GpuSurface {
  public:
      virtual ~GpuSurface();
      virtual GpuSurfaceDesc desc() const = 0;
      virtual bool isValid() const = 0;
      // Opaque platform handle for the import edge (an IOSurfaceRef on Apple,
      // an ID3D11Texture2D* on Windows). void* keeps the shared header neutral;
      // the importer/RHI context downcast in their platform .mm/.cpp.
      virtual void* nativeHandle() const = 0;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpusurface.cpp`:

```cpp
// GpuSurface is the concrete type behind the Phase-1 opaque forward decl. The
// shared header must stay platform-neutral; this test compiles it with no Apple
// headers and exercises the desc/validity contract through a tiny fake.
#include <QtTest>

#include "playback/gpu/gpusurface.h"
#include "playback/output/framepixelformat.h"

namespace {
class FakeGpuSurface : public GpuSurface {
public:
    explicit FakeGpuSurface(GpuSurfaceDesc d, bool valid) : m_desc(d), m_valid(valid) {}
    GpuSurfaceDesc desc() const override { return m_desc; }
    bool isValid() const override { return m_valid; }
    void* nativeHandle() const override { return m_valid ? this : nullptr; }
private:
    GpuSurfaceDesc m_desc;
    bool m_valid;
};
}  // namespace

class TestGpuSurface : public QObject {
    Q_OBJECT
private slots:
    void descRoundTrips();
    void invalidSurfaceHasNullHandle();
};

void TestGpuSurface::descRoundTrips() {
    FakeGpuSurface s({FramePixelFormat::Nv12, 1920, 1080}, true);
    QCOMPARE(s.desc().format, FramePixelFormat::Nv12);
    QCOMPARE(s.desc().width, 1920);
    QCOMPARE(s.desc().height, 1080);
    QVERIFY(s.isValid());
    QVERIFY(s.nativeHandle() != nullptr);
}

void TestGpuSurface::invalidSurfaceHasNullHandle() {
    FakeGpuSurface s({FramePixelFormat::Nv12, 0, 0}, false);
    QVERIFY(!s.isValid());
    QVERIFY(s.nativeHandle() == nullptr);
}

QTEST_GUILESS_MAIN(TestGpuSurface)
#include "tst_gpusurface.moc"
```

Register in `tests/unit/CMakeLists.txt`, immediately after the `tst_framehandle` line (Phase 1 added it after `tst_outputbusengine`):

```cmake
olr_add_unit_test(tst_gpusurface olr_test_playback)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpusurface
```
Expected: FAIL to compile — `playback/gpu/gpusurface.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/gpu/gpusurface.h`:

```cpp
#ifndef OLR_GPUSURFACE_H
#define OLR_GPUSURFACE_H

#include "playback/output/framepixelformat.h"

// A GPU-resident pixel surface (the concrete type behind the opaque
// `class GpuSurface;` forward-declared in playback/output/framehandle.h). It
// retains its native backing (an IOSurface-backed CVPixelBuffer on Apple, an
// ID3D11Texture2D on Windows) until every fence referencing it has retired.
//
// This header is intentionally PLATFORM-NEUTRAL: it leaks no CVPixelBuffer /
// Metal / D3D types. The native backing is exposed only as an opaque void* the
// platform importer/RHI context downcasts. That keeps the cross-platform import
// edge interface symmetric (D1/D8) and lets non-GPU translation units include
// this header without Apple/Windows SDK dependencies.

struct GpuSurfaceDesc {
    FramePixelFormat format = FramePixelFormat::Nv12;
    int width = 0;
    int height = 0;
};

class GpuSurface {
public:
    virtual ~GpuSurface();
    virtual GpuSurfaceDesc desc() const = 0;
    virtual bool isValid() const = 0;
    // Opaque native handle for the import edge: an IOSurfaceRef on Apple, an
    // ID3D11Texture2D* on Windows. Downcast only in platform .mm/.cpp.
    virtual void* nativeHandle() const = 0;
};

#endif  // OLR_GPUSURFACE_H
```

Create `playback/gpu/gpusurface_stub.cpp` (the out-of-line dtor; non-Apple platforms compile this so the vtable has a home):

```cpp
#include "playback/gpu/gpusurface.h"

GpuSurface::~GpuSurface() = default;
```

Create `playback/gpu/gpusurface_apple.mm` — Apple compiles this instead of the stub; the dtor lives here so the Apple `GpuSurface` concrete (Task 3) shares the vtable home:

```objcpp
#include "playback/gpu/gpusurface.h"

#ifdef __APPLE__

GpuSurface::~GpuSurface() = default;

#endif  // __APPLE__
```

Add `playback/gpu/*` to the engine source list in `CMakeLists.txt`. The engine `target_sources` already lists `playback/output/framehandle.{h,cpp}` (Phase 1). Add the GPU sources immediately after, choosing the backend per platform. In the `if(APPLE)` `target_sources(OpenLiveReplay PRIVATE ...)` block (currently at lines 347-353) append:

```cmake
        playback/gpu/gpusurface.h playback/gpu/gpusurface_apple.mm
```

In the `elseif(WIN32)` block (lines 363-369) and the `elseif(NOT WIN32)` block append:

```cmake
        playback/gpu/gpusurface.h playback/gpu/gpusurface_stub.cpp
```

> The unit-test library `olr_test_playback` must also see a `GpuSurface` vtable home. On Apple, `tst_gpusurface` only uses the header + the out-of-line dtor; link the dtor via an Apple-only backend lib (Task 3 introduces `olr_test_gpu`). For Task 1 in isolation, add `playback/gpu/gpusurface_stub.cpp` to `olr_test_playback`'s sources so the dtor resolves without Apple frameworks; Task 3 flips the Apple branch to the real backend lib. Locate `olr_test_playback`'s source list in `tests/unit/CMakeLists.txt` and add `${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_stub.cpp`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_gpusurface && ctest --test-dir build/c -R tst_gpusurface --output-on-failure
```
Expected: PASS (2 tests). (Reconfigure because new engine/test sources were added.)

- [ ] **Step 5: Verify the zero-regression gate**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: the full unit suite PASSES unchanged (GpuSurface is additive; nothing consumes it yet).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'  # format changed lines, stage first
git add playback/gpu/gpusurface.h playback/gpu/gpusurface_apple.mm playback/gpu/gpusurface_stub.cpp \
        tests/unit/tst_gpusurface.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-abstraction): platform-neutral GpuSurface concrete + stub"
```

---

## Task 2: `OLR_GPU_PIPELINE` flag gate + micro-stress hooks (`gpuPipelineEnabled`)

**Precondition:** none.

**Files:**
- Create: `playback/gpu/gpupipelineconfig.h`, `playback/gpu/gpupipelineconfig.cpp`
- Test: `tests/unit/tst_gpusurface.cpp` (add slots — same test exe, keep the GPU config near the surface tests)
- Modify: `CMakeLists.txt` (add the .cpp to all three platform branches)

**Interfaces:**
- Produces:
  ```cpp
  // playback/gpu/gpupipelineconfig.h
  // True when OLR_GPU_PIPELINE is set to "1"/"true"/"on" (case-insensitive).
  bool gpuPipelineEnabled();
  // Micro-stress injection knobs (§8): a forced tiny per-track frame budget and
  // a one-shot GPU-alloc-failure injection. Read by the worker/importer ONLY
  // when gpuPipelineEnabled(). Default: no override, no injected failure.
  int gpuForcedPerTrackBudget();       // -1 = no override; else cap per track
  bool gpuConsumeInjectedAllocFailure();  // returns true ONCE then resets (test-only)
  void gpuSetInjectedAllocFailures(int count);  // arm N one-shot failures (test-only)
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpusurface.cpp`:

```cpp
    void pipelineFlagDefaultsOff();
    void injectedAllocFailureIsOneShot();
```

```cpp
void TestGpuSurface::pipelineFlagDefaultsOff() {
    qunsetenv("OLR_GPU_PIPELINE");
    QVERIFY(!gpuPipelineEnabled());
    qputenv("OLR_GPU_PIPELINE", "1");
    QVERIFY(gpuPipelineEnabled());
    qputenv("OLR_GPU_PIPELINE", "0");
    QVERIFY(!gpuPipelineEnabled());
    qunsetenv("OLR_GPU_PIPELINE");
}

void TestGpuSurface::injectedAllocFailureIsOneShot() {
    gpuSetInjectedAllocFailures(2);
    QVERIFY(gpuConsumeInjectedAllocFailure());   // 1st
    QVERIFY(gpuConsumeInjectedAllocFailure());   // 2nd
    QVERIFY(!gpuConsumeInjectedAllocFailure());  // exhausted
}
```

Add the include at the top of the file:

```cpp
#include "playback/gpu/gpupipelineconfig.h"
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpusurface
```
Expected: FAIL to compile — `gpuPipelineEnabled` / `gpuConsumeInjectedAllocFailure` undeclared.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpupipelineconfig.h`:

```cpp
#ifndef OLR_GPUPIPELINECONFIG_H
#define OLR_GPUPIPELINECONFIG_H

// The single capability gate for the GPU-resident pipeline. OLR_GPU_PIPELINE is
// OFF by default; the CPU path is the permanent default + correctness oracle.
// Every GPU code path is reached only when gpuPipelineEnabled() AND a
// GpuRhiContext constructed successfully (Task 4).
bool gpuPipelineEnabled();

// Micro-stress injection (§8): the Phase-2 slice surfaces evict-while-render and
// OOM-degrade on a single feed by forcing a tiny per-track frame budget and a
// one-shot GPU-alloc failure. These are test-only knobs read in GPU paths only.
int gpuForcedPerTrackBudget();           // -1 = no override
bool gpuConsumeInjectedAllocFailure();   // true ONCE per armed failure, then resets
void gpuSetInjectedAllocFailures(int count);

#endif  // OLR_GPUPIPELINECONFIG_H
```

Create `playback/gpu/gpupipelineconfig.cpp`:

```cpp
#include "playback/gpu/gpupipelineconfig.h"

#include <QByteArray>

#include <atomic>

namespace {
std::atomic<int> g_injectedAllocFailures{0};
}  // namespace

bool gpuPipelineEnabled() {
    const QByteArray v = qgetenv("OLR_GPU_PIPELINE").toLower();
    return v == "1" || v == "true" || v == "on";
}

int gpuForcedPerTrackBudget() {
    const QByteArray v = qgetenv("OLR_GPU_FORCE_BUDGET");
    if (v.isEmpty()) return -1;
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : -1;
}

bool gpuConsumeInjectedAllocFailure() {
    int cur = g_injectedAllocFailures.load(std::memory_order_acquire);
    while (cur > 0) {
        if (g_injectedAllocFailures.compare_exchange_weak(cur, cur - 1,
                                                          std::memory_order_acq_rel))
            return true;
    }
    return false;
}

void gpuSetInjectedAllocFailures(int count) {
    g_injectedAllocFailures.store(count < 0 ? 0 : count, std::memory_order_release);
}
```

Add `playback/gpu/gpupipelineconfig.{h,cpp}` to **all three** platform `target_sources` blocks in `CMakeLists.txt` (Apple, WIN32, NOT WIN32) alongside the `gpusurface` entries from Task 1, e.g. on Apple:

```cmake
        playback/gpu/gpupipelineconfig.h playback/gpu/gpupipelineconfig.cpp
```

Also add `${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp` to `olr_test_playback`'s sources in `tests/unit/CMakeLists.txt`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_gpusurface && ctest --test-dir build/c -R tst_gpusurface --output-on-failure
```
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpupipelineconfig.h playback/gpu/gpupipelineconfig.cpp \
        tests/unit/tst_gpusurface.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-abstraction): OLR_GPU_PIPELINE gate + micro-stress injection hooks"
```

---

## Task 3: Apple `GpuSurface` concrete (IOSurface-backed CVPixelBuffer, RAII)

**Precondition:** P0.1 (needs an IOSurface-backed `CVPixelBuffer`). **If P0.1 failed:** still build this — Task 3 allocates its *own* IOSurface-backed buffer via `CVPixelBufferCreate` with `kCVPixelBufferIOSurfacePropertiesKey`, independent of whether VT can be coaxed into producing one. Only Task 4/6's VT *import* depends on P0.1.

**Files:**
- Create: `playback/gpu/appleiosurface.h`, `playback/gpu/applegpusurface_apple.mm`
- Test: `tests/unit/tst_gpusurface.cpp` (Apple-only slots, guarded by `#ifdef __APPLE__`)
- Modify: `tests/unit/CMakeLists.txt` (introduce the Apple-only `olr_test_gpu` backend lib), `CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuSurface`, `GpuSurfaceDesc` (Task 1).
- Produces:
  ```cpp
  // playback/gpu/appleiosurface.h — Apple-only factory, NO CVPixelBuffer in the
  // signature (returns the neutral base). Defined in applegpusurface_apple.mm.
  #ifdef __APPLE__
  // Allocate an IOSurface-backed NV12 GpuSurface (zero-initialized). Returns
  // nullptr on alloc failure or when the injected-alloc-failure knob fires.
  std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height);
  // Wrap an existing CVImageBufferRef (from VT decode) WITHOUT copying pixels;
  // retains it for the surface's lifetime. Passed as void* to keep the header
  // neutral; the .mm bridges it to CVImageBufferRef.
  std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef);
  #endif
  ```

- [ ] **Step 1: Write the failing test (Apple-only)**

Add to `tests/unit/tst_gpusurface.cpp`, guarded so non-Apple still compiles:

```cpp
#ifdef __APPLE__
    void appleSurfaceIsIoSurfaceBacked();
    void appleSurfaceRespectsInjectedAllocFailure();
#endif
```

```cpp
#ifdef __APPLE__
void TestGpuSurface::appleSurfaceIsIoSurfaceBacked() {
    auto s = makeAppleNv12Surface(64, 48);
    QVERIFY(s != nullptr);
    QVERIFY(s->isValid());
    QCOMPARE(s->desc().format, FramePixelFormat::Nv12);
    QCOMPARE(s->desc().width, 64);
    QCOMPARE(s->desc().height, 48);
    // nativeHandle() exposes the IOSurfaceRef (non-null for an IOSurface-backed
    // buffer); a CPU-only CVPixelBuffer would return null here.
    QVERIFY(s->nativeHandle() != nullptr);
}

void TestGpuSurface::appleSurfaceRespectsInjectedAllocFailure() {
    gpuSetInjectedAllocFailures(1);
    auto fail = makeAppleNv12Surface(64, 48);  // consumes the injected failure
    QVERIFY(fail == nullptr);
    auto ok = makeAppleNv12Surface(64, 48);    // next call succeeds
    QVERIFY(ok != nullptr);
}
#endif
```

Add the include:

```cpp
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif
```

- [ ] **Step 2: Introduce the Apple `olr_test_gpu` backend lib and flip `olr_test_playback`**

In `tests/unit/CMakeLists.txt`, after the `olr_add_unit_test(tst_gpusurface olr_test_playback)` line, add an Apple-only backend lib mirroring the `olr_test_nativevideoencoder` pattern (the H.264 encoder plan, Task 2). Remove the `gpusurface_stub.cpp`/`gpupipelineconfig.cpp` direct adds to `olr_test_playback` from Tasks 1-2 on Apple and route them through this lib:

```cmake
if(APPLE)
    add_library(olr_test_gpu STATIC
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_apple.mm"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/applegpusurface_apple.mm")
    target_include_directories(olr_test_gpu PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_gpu
        PUBLIC Qt6::Core
               "-framework CoreVideo" "-framework CoreFoundation"
               "-framework IOSurface" "-framework CoreMedia")
    target_link_libraries(tst_gpusurface PRIVATE olr_test_gpu)
else()
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_stub.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp")
endif()
```

> Keep `olr_test_playback`'s direct add of these `.cpp`s ONLY in the non-Apple branch (Apple now links them via `olr_test_gpu`), so no duplicate-symbol clash.

- [ ] **Step 3: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_gpusurface
```
Expected: FAIL — `playback/gpu/appleiosurface.h` not found / `makeAppleNv12Surface` undeclared.

- [ ] **Step 4: Write the implementation**

Create `playback/gpu/appleiosurface.h`:

```cpp
#ifndef OLR_APPLEIOSURFACE_H
#define OLR_APPLEIOSURFACE_H

#ifdef __APPLE__

#include "playback/gpu/gpusurface.h"

#include <memory>

// Allocate an IOSurface-backed NV12 GpuSurface (zero-initialized). Returns
// nullptr on CVPixelBufferCreate failure or when the injected-alloc-failure
// micro-stress knob fires. The CVPixelBuffer/IOSurface types are confined to the
// .mm; callers see only the neutral GpuSurface base.
std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height);

// Wrap an existing VT-decoded CVImageBufferRef (passed as void* to keep this
// header free of CoreVideo types) WITHOUT copying pixels; retains it for the
// surface's lifetime (the §4 surface-lifetime contract). Returns nullptr if the
// buffer is not IOSurface-backed.
std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef);

#endif  // __APPLE__
#endif  // OLR_APPLEIOSURFACE_H
```

Create `playback/gpu/applegpusurface_apple.mm`:

```objcpp
#include "playback/gpu/appleiosurface.h"

#ifdef __APPLE__

#include "playback/gpu/gpupipelineconfig.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

namespace {

class AppleGpuSurface : public GpuSurface {
public:
    // Takes ownership of a +1 retain on pb.
    explicit AppleGpuSurface(CVPixelBufferRef pb) : m_pb(pb) {}
    ~AppleGpuSurface() override {
        if (m_pb) CVPixelBufferRelease(m_pb);
    }

    GpuSurfaceDesc desc() const override {
        GpuSurfaceDesc d;
        d.format = FramePixelFormat::Nv12;
        if (m_pb) {
            d.width = int(CVPixelBufferGetWidth(m_pb));
            d.height = int(CVPixelBufferGetHeight(m_pb));
        }
        return d;
    }
    bool isValid() const override { return m_pb != nullptr && nativeHandle() != nullptr; }
    void* nativeHandle() const override {
        return m_pb ? CVPixelBufferGetIOSurface(m_pb) : nullptr;
    }
    CVPixelBufferRef pixelBuffer() const { return m_pb; }

private:
    CVPixelBufferRef m_pb = nullptr;
};

}  // namespace

std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    if (gpuConsumeInjectedAllocFailure()) return nullptr;  // micro-stress OOM injection

    CVPixelBufferRef pb = nullptr;
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* vals[] = {(__bridge const void*)@{}};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const CVReturn r =
        CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb);
    if (attrs) CFRelease(attrs);
    if (r != kCVReturnSuccess || !pb) return nullptr;
    auto s = std::make_shared<AppleGpuSurface>(pb);  // takes the +1 from Create
    return s->isValid() ? std::static_pointer_cast<GpuSurface>(s) : nullptr;
}

std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef) {
    if (!cvImageBufferRef) return nullptr;
    auto pb = static_cast<CVPixelBufferRef>(cvImageBufferRef);
    if (!CVPixelBufferGetIOSurface(pb)) return nullptr;  // not IOSurface-backed
    CVPixelBufferRetain(pb);  // retain for the surface's lifetime (§4 contract)
    auto s = std::make_shared<AppleGpuSurface>(pb);
    return s->isValid() ? std::static_pointer_cast<GpuSurface>(s) : nullptr;
}

#endif  // __APPLE__
```

Add `playback/gpu/applegpusurface_apple.mm` + `playback/gpu/appleiosurface.h` to the `if(APPLE)` `target_sources` block in `CMakeLists.txt`, and add `-framework IOSurface` to the Apple `target_link_libraries` block (CoreVideo/CoreMedia/CoreFoundation are already linked there).

- [ ] **Step 5: Run the test, expect PASS (on a Mac)**

```sh
cmake --build build/c --target tst_gpusurface && ctest --test-dir build/c -R tst_gpusurface --output-on-failure
```
Expected: PASS (6 tests on Apple — the IOSurface-backed assertions run; the injected-failure path returns null then succeeds).

- [ ] **Step 6: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/appleiosurface.h playback/gpu/applegpusurface_apple.mm \
        tests/unit/tst_gpusurface.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-abstraction): IOSurface-backed Apple GpuSurface (RAII, OOM-injectable)"
```

---

## Task 4: `GpuRhiContext` — one QRhi on a dedicated render thread (D11)

**Precondition:** P0.3 (RHI overhead) + P0.5 (RHI↔IOSurface interop). **If P0.5 failed:** keep `GpuRhiContext` for the render path but make `importAndReadback` fall back to a `CVPixelBufferLockBaseAddress` download (Task 5 uses it). **If RHI cannot initialize at all on the host** (`offscreen`/CI without a Metal device): `GpuRhiContext::create` returns nullptr and the whole GPU path degrades to CPU — the slice still runs with `OLR_GPU_PIPELINE=1` by falling back, asserted in Task 4's degrade test.

**Files:**
- Create: `playback/gpu/gpurhicontext.h`, `playback/gpu/gpurhicontext_apple.mm`
- Test: `tests/unit/tst_gpurhicontext.cpp`
- Modify: `tests/unit/CMakeLists.txt` (register; add `Qt6::GuiPrivate` + Metal frameworks to `olr_test_gpu`), `CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuSurface` (Task 1), `CpuPlanes`/`FramePixelFormat` (Phase 1).
- Produces:
  ```cpp
  // playback/gpu/gpurhicontext.h — neutral header; QRhi is Qt, not platform SDK,
  // but the Metal init params are confined to the .mm.
  class GpuRhiContext {
  public:
      // Construct the single QRhi (Metal on Apple) + spawn the dedicated render
      // thread (D11). Returns nullptr if no GPU/RHI backend is available — the
      // caller then degrades to the CPU path.
      static std::shared_ptr<GpuRhiContext> create();
      ~GpuRhiContext();
      bool isValid() const;
      // Import a GpuSurface as an RHI texture, run a no-op offscreen pass
      // (beginOffscreenFrame/endOffscreenFrame), and read it back to CpuPlanes in
      // `target`. Runs on the dedicated render thread; blocks the caller until
      // the readback retires. Returns an empty CpuPlanes on failure (caller
      // degrades). This is the lazy readToCpu chokepoint for GpuFrameData (Task 5).
      CpuPlanes importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                  FramePixelFormat target);
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpurhicontext.cpp`:

```cpp
// GpuRhiContext owns one QRhi on a dedicated render thread (D11). On a GPU host
// it imports an IOSurface-backed surface and reads it back; with no RHI backend
// it returns nullptr and the caller degrades to CPU. The render-thread affinity
// is exercised by importAndReadback being callable from the test (caller) thread.
#include <QtTest>

#include "playback/gpu/gpurhicontext.h"
#include "playback/output/framepixelformat.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif

class TestGpuRhiContext : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
#ifdef __APPLE__
    void importAndReadbackProducesPlanes();
#endif
};

void TestGpuRhiContext::createIsNullOrValidNeverPartial() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host (expected under offscreen/CI)");
    QVERIFY(ctx->isValid());
}

#ifdef __APPLE__
void TestGpuRhiContext::importAndReadbackProducesPlanes() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    const CpuPlanes planes = ctx->importAndReadback(surface, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 64);
    QCOMPARE(planes.height, 48);
    QVERIFY(planes.isValid());
}
#endif

QTEST_GUILESS_MAIN(TestGpuRhiContext)
#include "tst_gpurhicontext.moc"
```

Register in `tests/unit/CMakeLists.txt` after `tst_gpusurface`:

```cmake
olr_add_unit_test(tst_gpurhicontext olr_test_playback)
if(APPLE)
    target_link_libraries(tst_gpurhicontext PRIVATE olr_test_gpu)
endif()
```

And extend `olr_test_gpu` (Apple) to compile `gpurhicontext_apple.mm` and link `Qt6::GuiPrivate` + Metal:

```cmake
    target_sources(olr_test_gpu PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_apple.mm")
    target_link_libraries(olr_test_gpu PUBLIC Qt6::GuiPrivate "-framework Metal" "-framework QuartzCore")
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_gpurhicontext
```
Expected: FAIL — `playback/gpu/gpurhicontext.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpurhicontext.h`:

```cpp
#ifndef OLR_GPURHICONTEXT_H
#define OLR_GPURHICONTEXT_H

#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"  // CpuPlanes, FramePixelFormat

#include <memory>

// Owns the single QRhi (Metal on Apple, D3D on Windows) on a dedicated render
// thread (D11): a QRhi is single-threaded and its textures have thread affinity,
// so every QRhi call funnels through that one thread. The fenced producer/
// consumer model (D4, gpu-sync) rides on top of this single-render-thread RHI.
class GpuRhiContext {
public:
    // Returns nullptr when no GPU/RHI backend is available (headless offscreen,
    // CI runners with no Metal device); the caller degrades to the CPU path.
    static std::shared_ptr<GpuRhiContext> create();
    ~GpuRhiContext();

    GpuRhiContext(const GpuRhiContext&) = delete;
    GpuRhiContext& operator=(const GpuRhiContext&) = delete;

    bool isValid() const;

    // Import `surface` as an RHI texture, run a no-op offscreen pass, and read it
    // back to CpuPlanes in `target` on the render thread. Blocks the caller until
    // the readback retires. Empty CpuPlanes on failure (caller degrades).
    CpuPlanes importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                FramePixelFormat target);

private:
    class Impl;
    explicit GpuRhiContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

#endif  // OLR_GPURHICONTEXT_H
```

Create `playback/gpu/gpurhicontext_apple.mm`. The Impl owns a `GpuRenderThread` (a `QThread`) and a `QRhi` created with `QRhiMetalInitParams`; `importAndReadback` posts a job to the render thread and waits. The IOSurface→Metal import uses `CVMetalTextureCache` and `QRhiTexture::createFrom` (P0.5); on P0.5 failure it locks the `CVPixelBuffer` and copies. Keep the no-op offscreen pass minimal (begin/end frame + a texture readback) — the compositor shader lands in Phase 3 (`gpu-compositor`):

```objcpp
#include "playback/gpu/gpurhicontext.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"

#include <QtGui/rhi/qrhi.h>

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <functional>

namespace {

// Read an IOSurface-backed NV12 CVPixelBuffer (the surface's nativeHandle
// downcasts to its parent CVPixelBuffer via the lock path) down to Yuv420p
// CpuPlanes. P0.5-fallback path: lock + deinterleave, byte-identical to the
// existing nativevideodecoder copyPixelBufferToAvFrame (NV12->I420) so the
// ±1-LSB gate holds. The RHI readback path (when P0.5 holds) produces the same
// bytes; this fallback is the correctness floor.
CpuPlanes lockDownloadNv12ToYuv420p(CVPixelBufferRef pb) {
    CpuPlanes out;
    if (!pb) return out;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return out;
    const int w = int(CVPixelBufferGetWidth(pb));
    const int h = int(CVPixelBufferGetHeight(pb));
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    out.format = FramePixelFormat::Yuv420p;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = cw;
    out.stride[2] = cw;
    out.plane[0] = QByteArray(w * h, Qt::Uninitialized);
    out.plane[1] = QByteArray(cw * ch, Qt::Uninitialized);
    out.plane[2] = QByteArray(cw * ch, Qt::Uninitialized);

    const auto* ySrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    for (int y = 0; y < h; ++y)
        memcpy(out.plane[0].data() + y * w, ySrc + y * yStride, size_t(w));

    const auto* uvSrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    char* u = out.plane[1].data();
    char* v = out.plane[2].data();
    for (int row = 0; row < ch; ++row) {
        const uchar* s = uvSrc + row * uvStride;
        for (int x = 0; x < cw; ++x) {
            u[row * cw + x] = char(s[2 * x]);
            v[row * cw + x] = char(s[2 * x + 1]);
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return out;
}

// The dedicated render thread (D11): all QRhi calls run here.
class GpuRenderThread : public QThread {
public:
    QRhi* rhi = nullptr;
    void run() override {
        QRhiMetalInitParams params;  // default device
        rhi = QRhi::create(QRhi::Metal, &params);
        // Signal readiness, then service posted jobs until asked to stop.
        {
            QMutexLocker lk(&m_mutex);
            m_ready = true;
            m_cond.wakeAll();
        }
        forever {
            std::function<void()> job;
            {
                QMutexLocker lk(&m_mutex);
                while (m_jobs.isEmpty() && !m_stop) m_cond.wait(&m_mutex);
                if (m_stop && m_jobs.isEmpty()) break;
                job = m_jobs.takeFirst();
            }
            job();
        }
        delete rhi;
        rhi = nullptr;
    }
    bool waitReady() {
        QMutexLocker lk(&m_mutex);
        while (!m_ready) m_cond.wait(&m_mutex);
        return rhi != nullptr;
    }
    void post(std::function<void()> job) {  // blocks the caller until job runs
        QMutexLocker lk(&m_mutex);
        bool done = false;
        m_jobs.append([&] { job(); QMutexLocker l(&m_mutex); done = true; m_cond.wakeAll(); });
        m_cond.wakeAll();
        while (!done) m_cond.wait(&m_mutex);
    }
    void requestStop() {
        QMutexLocker lk(&m_mutex);
        m_stop = true;
        m_cond.wakeAll();
    }
private:
    QMutex m_mutex;
    QWaitCondition m_cond;
    QList<std::function<void()>> m_jobs;
    bool m_ready = false;
    bool m_stop = false;
};

}  // namespace

class GpuRhiContext::Impl {
public:
    GpuRenderThread thread;
};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GpuRhiContext::~GpuRhiContext() {
    if (m_impl) {
        m_impl->thread.requestStop();
        m_impl->thread.wait();
    }
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
    auto impl = std::make_unique<Impl>();
    impl->thread.start();
    if (!impl->thread.waitReady()) {  // QRhi::create returned null → no backend
        impl->thread.requestStop();
        impl->thread.wait();
        return nullptr;
    }
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::move(impl)));
}

bool GpuRhiContext::isValid() const { return m_impl && m_impl->thread.rhi != nullptr; }

CpuPlanes GpuRhiContext::importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                           FramePixelFormat target) {
    CpuPlanes result;
    if (!surface || !surface->isValid() || target != FramePixelFormat::Yuv420p) return result;
    // The IOSurface's parent CVPixelBuffer: wrapAppleImageBuffer keeps it; here we
    // recover it from the surface for the readback. (The Apple GpuSurface exposes
    // the IOSurface via nativeHandle; the lock-download recreates a CVPixelBuffer
    // view of that IOSurface.)
    IOSurfaceRef io = static_cast<IOSurfaceRef>(surface->nativeHandle());
    if (!io) return result;
    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, io, nullptr, &pb) != kCVReturnSuccess
        || !pb)
        return result;
    // The render thread runs the begin/endOffscreenFrame pass (no-op shader this
    // phase); the readback itself is the lock-download (P0.5 fallback / oracle).
    m_impl->thread.post([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (rhi) {
            QRhiCommandBuffer* cb = nullptr;
            if (rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess)
                rhi->endOffscreenFrame();
        }
        result = lockDownloadNv12ToYuv420p(pb);
    });
    CVPixelBufferRelease(pb);
    return result;
}

#endif  // __APPLE__
```

> **Note on the readback path:** this phase deliberately uses the lock-download as the byte-exact oracle inside `importAndReadback` (it reproduces the existing `copyPixelBufferToAvFrame` NV12→I420 math so the ±1-LSB gate is exact), while still spinning the QRhi offscreen frame so the render-thread + RHI plumbing is exercised end-to-end on the real device. The QRhi import/readback path is NOT exercised for bytes here — the begin/endOffscreenFrame pass is a no-op and the CVPixelBuffer lock-download is the single readback chokepoint, so `copy-detector==1` still holds (one download per frame, not P0.5 validation). The zero-copy `CVMetalTextureCache`→`QRhiTexture::createFrom` readback is the `gpu-compositor` (Phase 3) optimization; promoting it here is gated on P0.5 and does not change the produced bytes.

Add a non-Apple stub so the type exists everywhere. Create the body inside `playback/gpu/gpurhicontext_stub.cpp` (`create()` → nullptr, `isValid()` → false, `importAndReadback` → empty) and add it to the WIN32 / NOT WIN32 `target_sources` (and `olr_test_playback` non-Apple sources). Mirror the structure of `gpusurface_stub.cpp`.

- [ ] **Step 4: Run the test, expect PASS (or SKIP under offscreen)**

```sh
cmake --build build/c --target tst_gpurhicontext && ctest --test-dir build/c -R tst_gpurhicontext --output-on-failure
```
Expected on a GPU-capable Mac: PASS (2 tests). Under `QT_QPA_PLATFORM=offscreen` with no Metal device the import test SKIPs and `createIsNullOrValidNeverPartial` SKIPs — never a hard FAIL.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm playback/gpu/gpurhicontext_stub.cpp \
        tests/unit/tst_gpurhicontext.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-abstraction): GpuRhiContext (one QRhi, dedicated render thread, D11)"
```

---

## Task 5: `GpuFrameData : IFrameData` — GPU-backed handle with lazy `readToCpu`

**Precondition:** P0.1 (a surface to wrap). Works regardless of P0.5 (readback path chosen inside `GpuRhiContext`).

**Files:**
- Create: `playback/gpu/gpuframedata.h`, `playback/gpu/gpuframedata.cpp`
- Test: `tests/unit/tst_gpuframedata.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `IFrameData`, `CpuPlanes`, `FramePixelFormat`, `FrameHandle`, `FrameMetadata`, `makeCpuFrameHandle` (Phase 1); `GpuSurface` (Task 1); `GpuRhiContext` (Task 4).
- Produces:
  ```cpp
  // playback/gpu/gpuframedata.h
  class GpuFrameData : public IFrameData {
  public:
      GpuFrameData(std::shared_ptr<GpuSurface> surface,
                   std::shared_ptr<GpuRhiContext> rhi,
                   FramePixelFormat nativeFormat);
      bool isGpuBacked() const override { return true; }
      CpuPlanes readToCpu(FramePixelFormat target) const override;   // lazy download
      GpuSurface* gpuSurface() const override;
      FramePixelFormat nativeFormat() const override;
      // Number of times a GPU->CPU download actually executed (telemetry-contract
      // copy-detector instrumentation): exposed for the gate.
      int readToCpuCount() const;
  };
  // Wrap a GPU surface into a GPU-backed FrameHandle with the given metadata.
  FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                                 std::shared_ptr<GpuRhiContext> rhi,
                                 FrameMetadata meta);
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpuframedata.cpp`:

```cpp
// GpuFrameData is the GPU concrete of IFrameData: isGpuBacked()==true,
// gpuSurface() non-null, readToCpu() lazily downloads exactly once per call and
// matches the CPU decode of the same pixels within +/-1 LSB.
#include <QtTest>

#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/framehandle.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif

class TestGpuFrameData : public QObject {
    Q_OBJECT
private slots:
    void gpuBackedReportsSurface();
#ifdef __APPLE__
    void readToCpuDownloadsAndCounts();
    void readbackMatchesCpuWithinOneLsb();
#endif
};

void TestGpuFrameData::gpuBackedReportsSurface() {
#ifdef __APPLE__
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata m;
    m.key.format = FramePixelFormat::Nv12;
    m.key.width = 64; m.key.height = 48;
    FrameHandle h = makeGpuFrameHandle(surface, rhi, m);
    QVERIFY(h.isGpuBacked());
    QVERIFY(h.data()->gpuSurface() != nullptr);
    QCOMPARE(h.data()->nativeFormat(), FramePixelFormat::Nv12);
#else
    QSKIP("GPU backend is Apple-only in Phase 2");
#endif
}

#ifdef __APPLE__
void TestGpuFrameData::readToCpuDownloadsAndCounts() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(64, 48);
    FrameMetadata m;
    m.key.format = FramePixelFormat::Nv12;
    m.key.width = 64; m.key.height = 48;
    FrameHandle h = makeGpuFrameHandle(surface, rhi, m);
    const auto* gd = dynamic_cast<const GpuFrameData*>(h.data());
    QVERIFY(gd != nullptr);
    QCOMPARE(gd->readToCpuCount(), 0);
    const CpuPlanes planes = h.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(planes.isValid());
    QCOMPARE(planes.width, 64);
    QCOMPARE(gd->readToCpuCount(), 1);  // exactly one download executed
}

void TestGpuFrameData::readbackMatchesCpuWithinOneLsb() {
    // A surface filled with a known NV12 value, read back to Yuv420p, must match
    // the CPU deinterleave within +/-1 LSB/channel (the §8 readback gate).
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(16, 16);  // zero-initialized NV12
    FrameMetadata m;
    m.key.format = FramePixelFormat::Nv12;
    m.key.width = 16; m.key.height = 16;
    FrameHandle h = makeGpuFrameHandle(surface, rhi, m);
    const CpuPlanes got = h.readToCpu(FramePixelFormat::Yuv420p);
    // Zero-initialized NV12: Y=0, interleaved UV=0 -> U=0, V=0 everywhere.
    for (char b : got.plane[0]) QVERIFY(qAbs(int(uchar(b)) - 0) <= 1);
    for (char b : got.plane[1]) QVERIFY(qAbs(int(uchar(b)) - 0) <= 1);
    for (char b : got.plane[2]) QVERIFY(qAbs(int(uchar(b)) - 0) <= 1);
}
#endif

QTEST_GUILESS_MAIN(TestGpuFrameData)
#include "tst_gpuframedata.moc"
```

Register in `tests/unit/CMakeLists.txt` after `tst_gpurhicontext`:

```cmake
olr_add_unit_test(tst_gpuframedata olr_test_playback)
if(APPLE)
    target_link_libraries(tst_gpuframedata PRIVATE olr_test_gpu)
endif()
```

And add `gpuframedata.cpp` to `olr_test_gpu` (Apple) and the non-Apple `olr_test_playback` GPU sources.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpuframedata
```
Expected: FAIL — `playback/gpu/gpuframedata.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/gpuframedata.h`:

```cpp
#ifndef OLR_GPUFRAMEDATA_H
#define OLR_GPUFRAMEDATA_H

#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <atomic>
#include <memory>

// The GPU concrete of IFrameData (Phase 1's CpuFrameData sibling). It RETAINS a
// GpuSurface for its whole lifetime (the §4 surface-lifetime contract) and
// downloads lazily: readToCpu() runs the GpuRhiContext import->readback only
// when a CPU sink asks. isGpuBacked() is true; gpuSurface() is non-null.
class GpuFrameData : public IFrameData {
public:
    GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                 FramePixelFormat nativeFmt);
    ~GpuFrameData() override;

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return m_nativeFormat; }

    // Count of GPU->CPU downloads actually executed (telemetry-contract
    // copy-on-GPU-path detector). One per unique rendered bus surface is the gate.
    int readToCpuCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    std::shared_ptr<GpuSurface> m_surface;
    std::shared_ptr<GpuRhiContext> m_rhi;
    FramePixelFormat m_nativeFormat;
    mutable std::atomic<int> m_readCount{0};
};

// Wrap a GPU surface into a GPU-backed FrameHandle.
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta);

#endif  // OLR_GPUFRAMEDATA_H
```

Create `playback/gpu/gpuframedata.cpp`:

```cpp
#include "playback/gpu/gpuframedata.h"

GpuFrameData::GpuFrameData(std::shared_ptr<GpuSurface> surface,
                           std::shared_ptr<GpuRhiContext> rhi, FramePixelFormat nativeFmt)
    : m_surface(std::move(surface)), m_rhi(std::move(rhi)), m_nativeFormat(nativeFmt) {}

GpuFrameData::~GpuFrameData() = default;

CpuPlanes GpuFrameData::readToCpu(FramePixelFormat target) const {
    if (!m_surface || !m_rhi) return CpuPlanes{};
    CpuPlanes planes = m_rhi->importAndReadback(m_surface, target);
    if (planes.isValid()) m_readCount.fetch_add(1, std::memory_order_acq_rel);
    return planes;
}

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta) {
    auto data = std::make_shared<GpuFrameData>(std::move(surface), std::move(rhi), meta.key.format);
    return FrameHandle(std::move(data), std::move(meta));
}
```

Add `playback/gpu/gpuframedata.{h,cpp}` to all three platform `target_sources` blocks in `CMakeLists.txt` (it is platform-neutral — it only includes the neutral GPU headers).

- [ ] **Step 4: Run the test, expect PASS (or SKIP under offscreen)**

```sh
cmake --build build/c --target tst_gpuframedata && ctest --test-dir build/c -R tst_gpuframedata --output-on-failure
```
Expected on a GPU Mac: PASS (3 tests), including the ±1-LSB readback gate and the one-download-per-call count. Under offscreen: SKIPs.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp \
        tests/unit/tst_gpuframedata.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-abstraction): GpuFrameData IFrameData concrete + lazy readToCpu + copy counter"
```

---

## Task 6: `DecodeDoneFence` — the minimal `gpu-sync` primitive (MTLSharedEvent)

**Precondition:** none (Metal `MTLSharedEvent` is always available on Apple). **Branch:** non-Apple gets a no-op fence that is always "signaled".

**Files:**
- Create: `playback/gpu/decodedonefence.h`, `playback/gpu/decodedonefence_apple.mm`, `playback/gpu/decodedonefence_stub.cpp`
- Test: `tests/unit/tst_decodedonefence.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  // playback/gpu/decodedonefence.h — the first gpu-sync primitive: a worker
  // signals decode-done before publishing a GPU handle; a consumer (eviction,
  // readback) waits before touching the surface. Phase 2 uses it ONLY to make the
  // micro-stress (Task 8) exercise eviction against a real fence, not a fenceless
  // approximation. The full generation-counter/eviction-guard contract is gpu-sync.
  class DecodeDoneFence {
  public:
      static std::shared_ptr<DecodeDoneFence> create();  // null if unavailable
      virtual ~DecodeDoneFence();
      virtual void signalDecodeDone() = 0;   // producer: decode finished
      virtual bool waitDecodeDone(int timeoutMs) = 0;  // consumer: block until signaled
      virtual bool isSignaled() const = 0;
  };
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_decodedonefence.cpp`:

```cpp
// DecodeDoneFence: a producer signals decode-done; a consumer waits. The
// micro-stress (Task 8) uses it so eviction waits on a real fence before freeing
// a surface. Cross-thread signal/wait is the contract that matters.
#include <QtTest>

#include "playback/gpu/decodedonefence.h"

#include <thread>

class TestDecodeDoneFence : public QObject {
    Q_OBJECT
private slots:
    void signalThenWaitSucceeds();
    void waitBlocksUntilSignaledFromAnotherThread();
};

void TestDecodeDoneFence::signalThenWaitSucceeds() {
    auto f = DecodeDoneFence::create();
    if (!f) QSKIP("no fence backend on this platform");
    QVERIFY(!f->isSignaled());
    f->signalDecodeDone();
    QVERIFY(f->waitDecodeDone(1000));
    QVERIFY(f->isSignaled());
}

void TestDecodeDoneFence::waitBlocksUntilSignaledFromAnotherThread() {
    auto f = DecodeDoneFence::create();
    if (!f) QSKIP("no fence backend on this platform");
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        f->signalDecodeDone();
    });
    QVERIFY(f->waitDecodeDone(2000));  // returns only after the producer signals
    producer.join();
}

QTEST_GUILESS_MAIN(TestDecodeDoneFence)
#include "tst_decodedonefence.moc"
```

Register in `tests/unit/CMakeLists.txt` after `tst_gpuframedata`:

```cmake
olr_add_unit_test(tst_decodedonefence olr_test_playback)
if(APPLE)
    target_link_libraries(tst_decodedonefence PRIVATE olr_test_gpu)
endif()
```

Add `decodedonefence_apple.mm` to `olr_test_gpu` (Apple) and `decodedonefence_stub.cpp` to the non-Apple `olr_test_playback` sources.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_decodedonefence
```
Expected: FAIL — `playback/gpu/decodedonefence.h` not found.

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/decodedonefence.h`:

```cpp
#ifndef OLR_DECODEDONEFENCE_H
#define OLR_DECODEDONEFENCE_H

#include <memory>

// The minimal gpu-sync primitive shipped with the Phase-2 slice: a worker
// signals decode-done before publishing a GPU handle; a consumer (eviction or
// readback) waits before touching the surface. This is the FIRST piece of
// gpu-sync (D4); the full backend-matched fence + GPU generation counter +
// eviction guard land in the gpu-sync subproject (Phase 3). Apple uses an
// MTLSharedEvent; other platforms get an always-signaled no-op.
class DecodeDoneFence {
public:
    static std::shared_ptr<DecodeDoneFence> create();
    virtual ~DecodeDoneFence();
    virtual void signalDecodeDone() = 0;
    virtual bool waitDecodeDone(int timeoutMs) = 0;
    virtual bool isSignaled() const = 0;
};

#endif  // OLR_DECODEDONEFENCE_H
```

Create `playback/gpu/decodedonefence_apple.mm`:

```objcpp
#include "playback/gpu/decodedonefence.h"

#ifdef __APPLE__

#include <Metal/Metal.h>

#include <atomic>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {

class MetalDecodeDoneFence : public DecodeDoneFence {
public:
    explicit MetalDecodeDoneFence(id<MTLSharedEvent> ev) : m_event(ev) {}
    ~MetalDecodeDoneFence() override { m_event = nil; }

    void signalDecodeDone() override {
        m_event.signaledValue = 1;
        m_signaled.store(true, std::memory_order_release);
    }
    bool waitDecodeDone(int timeoutMs) override {
        // MTLSharedEvent::waitUntilSignaledValue is available on macOS 12+;
        // model the wait with a bounded poll so the primitive is testable from a
        // plain thread without a command queue. The gpu-sync subproject replaces
        // this with GPU-side encoder waits.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (!m_signaled.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    bool isSignaled() const override { return m_signaled.load(std::memory_order_acquire); }

private:
    id<MTLSharedEvent> m_event = nil;
    std::atomic<bool> m_signaled{false};
};

}  // namespace

std::shared_ptr<DecodeDoneFence> DecodeDoneFence::create() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nullptr;
    id<MTLSharedEvent> ev = [device newSharedEvent];
    if (!ev) return nullptr;
    return std::make_shared<MetalDecodeDoneFence>(ev);
}

#endif  // __APPLE__
```

Create `playback/gpu/decodedonefence_stub.cpp` (non-Apple: always-signaled no-op):

```cpp
#include "playback/gpu/decodedonefence.h"

#ifndef __APPLE__

#include <atomic>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {
class NoopDecodeDoneFence : public DecodeDoneFence {
public:
    void signalDecodeDone() override { m_signaled.store(true); }
    bool waitDecodeDone(int) override { return true; }  // no GPU work to wait on
    bool isSignaled() const override { return m_signaled.load(); }
private:
    std::atomic<bool> m_signaled{false};
};
}  // namespace

std::shared_ptr<DecodeDoneFence> DecodeDoneFence::create() {
    return std::make_shared<NoopDecodeDoneFence>();
}

#endif  // !__APPLE__
```

Add `decodedonefence.h` + the per-platform backend (`_apple.mm` on Apple, `_stub.cpp` elsewhere) to the platform `target_sources` blocks in `CMakeLists.txt`. The Apple `.mm` needs `-framework Metal` (already added in Task 4 to `OpenLiveReplay`'s Apple link? add if not present) plus `<chrono>`/`<thread>` includes — add `#include <chrono>` and `#include <thread>` to `decodedonefence_apple.mm`.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_decodedonefence && ctest --test-dir build/c -R tst_decodedonefence --output-on-failure
```
Expected: PASS (2 tests) — including the cross-thread signal/wait.

- [ ] **Step 5: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/decodedonefence.h playback/gpu/decodedonefence_apple.mm playback/gpu/decodedonefence_stub.cpp \
        tests/unit/tst_decodedonefence.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-sync): minimal DecodeDoneFence (MTLSharedEvent) for the Phase-2 slice"
```

---

## Task 7: VideoToolbox keep-surface import (P0.1 — the real import edge)

**Precondition:** P0.1. **If P0.1 failed:** see the branch note in Preconditions — wrap a CPU-uploaded surface and document copy-detector != 1 for the import edge.

**Files:**
- Modify: `recorder_engine/ingest/nativevideodecoder.h` (add a `KeepSurfaceCallback` + `decodeKeepSurface` entry point), `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` (request `kCVPixelBufferIOSurfacePropertiesKey`; deliver the `CVImageBufferRef`)
- Create: `playback/gpu/vtkeepsurfaceimporter.h`, `playback/gpu/vtkeepsurfaceimporter_apple.mm`
- Test: `tests/unit/tst_gpuframedata.cpp` (add an Apple-only import slot)
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `CompressedAccessUnit` (`recorder_engine/ingest/h26xaccessunit.h`), `NativeVideoDecoder` (existing); `wrapAppleImageBuffer` (Task 3); `GpuRhiContext` (Task 4); `makeGpuFrameHandle` (Task 5).
- Produces:
  ```cpp
  // nativevideodecoder.h additions:
  //   using KeepSurfaceCallback = std::function<void(void* cvImageBufferRef, qint64 pts90k)>;
  //   bool decodeKeepSurface(const CompressedAccessUnit&, KeepSurfaceCallback, QString*);
  // The session is created with kCVPixelBufferIOSurfacePropertiesKey so the
  // output CVPixelBuffer is IOSurface-backed (P0.1).
  //
  // playback/gpu/vtkeepsurfaceimporter.h:
  //   FrameHandle importVtImageBuffer(void* cvImageBufferRef, FrameMetadata meta,
  //                                   std::shared_ptr<GpuRhiContext> rhi);
  ```

- [ ] **Step 1: Write the failing test (Apple-only)**

Add to `tests/unit/tst_gpuframedata.cpp`:

```cpp
#ifdef __APPLE__
    void importVtBufferProducesGpuHandle();
#endif
```

```cpp
#ifdef __APPLE__
void TestGpuFrameData::importVtBufferProducesGpuHandle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    // Build an IOSurface-backed buffer to stand in for a VT decode output.
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata m;
    m.key.format = FramePixelFormat::Nv12;
    m.key.width = 64; m.key.height = 48;
    m.key.ptsMs = 40;
    // The test-only importVtSurface overload wraps an already-made GpuSurface so
    // the test exercises GPU-handle assembly without a live VT decode (the worker
    // passes VT's actual CVImageBufferRef to importVtImageBuffer in production).
    FrameHandle h = importVtSurface(surface, m, rhi);
    QVERIFY(h.isGpuBacked());
    QCOMPARE(h.metadata().key.format, FramePixelFormat::Nv12);
}
#endif
```

> Note: `importVtImageBuffer` expects a `CVImageBufferRef`. For the unit test, add a test-only overload `FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>&, FrameMetadata, std::shared_ptr<GpuRhiContext>)` in the importer header that wraps an already-made surface, and call that here instead, so the test does not need a live VT decode. Use that overload in the test body; the production path (Task 8) calls `importVtImageBuffer` with VT's actual buffer.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpuframedata
```
Expected: FAIL — `playback/gpu/vtkeepsurfaceimporter.h` not found.

- [ ] **Step 3: Add the keep-surface decode mode to the VT decoder**

In `recorder_engine/ingest/nativevideodecoder.h`, add (after the existing `FrameCallback` at line 23):

```cpp
    // Keep-surface mode (GPU-resident pipeline): instead of copying the decoded
    // CVPixelBuffer down to a CPU AVFrame, deliver the IOSurface-backed
    // CVImageBufferRef (as void*) so the caller can wrap it in a GPU-backed
    // FrameHandle. The buffer is valid only for the duration of the callback;
    // the caller retains it (CVPixelBufferRetain) to keep it alive.
    using KeepSurfaceCallback = std::function<void(void* cvImageBufferRef, qint64 pts90k)>;
    bool decodeKeepSurface(const CompressedAccessUnit& unit, KeepSurfaceCallback onSurface,
                           QString* error);
```

In `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm`:
- In `createSession` (line 324), add the IOSurface properties request so the output is IOSurface-backed (P0.1). After the pixel-format dictionary entry (line 329) add:

```cpp
    CFDictionaryRef ioSurfaceProps =
        CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurfaceProps);
    CFRelease(ioSurfaceProps);
```

- Add a `bool keepSurface` to `DecodeFrameContext` and a `KeepSurfaceCallback* surfaceCallback`. In `decompressionOutputCallback` (line 193), before `copyPixelBufferToAvFrame`, branch: if `context->keepSurface && context->surfaceCallback`, call `(*context->surfaceCallback)(imageBuffer, context->pts90k)` and set `emittedFrame = true` instead of copying.
- Add `Impl::decodeKeepSurface` mirroring `Impl::decode` (the block at lines 381-459) but setting `context.keepSurface = true; context.surfaceCallback = &onSurface;`. Add the public `NativeVideoDecoder::decodeKeepSurface` forwarding to `m_impl`.

- [ ] **Step 4: Write the importer**

Create `playback/gpu/vtkeepsurfaceimporter.h`:

```cpp
#ifndef OLR_VTKEEPSURFACEIMPORTER_H
#define OLR_VTKEEPSURFACEIMPORTER_H

#ifdef __APPLE__

#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <memory>

// Wrap a VideoToolbox-decoded CVImageBufferRef (IOSurface-backed, P0.1) into a
// GPU-backed FrameHandle WITHOUT copying pixels: the handle retains the surface
// for its lifetime (§4 surface-lifetime contract) and downloads lazily on
// readToCpu via the shared GpuRhiContext.
FrameHandle importVtImageBuffer(void* cvImageBufferRef, FrameMetadata meta,
                                std::shared_ptr<GpuRhiContext> rhi);

// Test/convenience overload: wrap an already-made GpuSurface (lets a unit test
// exercise the GPU-handle assembly without a live VT decode).
FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface, FrameMetadata meta,
                            std::shared_ptr<GpuRhiContext> rhi);

#endif  // __APPLE__
#endif  // OLR_VTKEEPSURFACEIMPORTER_H
```

Create `playback/gpu/vtkeepsurfaceimporter_apple.mm`:

```objcpp
#include "playback/gpu/vtkeepsurfaceimporter.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"

FrameHandle importVtImageBuffer(void* cvImageBufferRef, FrameMetadata meta,
                                std::shared_ptr<GpuRhiContext> rhi) {
    std::shared_ptr<GpuSurface> surface = wrapAppleImageBuffer(cvImageBufferRef);
    if (!surface) return FrameHandle();  // not IOSurface-backed -> caller degrades
    meta.key.format = FramePixelFormat::Nv12;
    return makeGpuFrameHandle(std::move(surface), std::move(rhi), std::move(meta));
}

FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface, FrameMetadata meta,
                            std::shared_ptr<GpuRhiContext> rhi) {
    if (!surface || !surface->isValid()) return FrameHandle();
    meta.key.format = FramePixelFormat::Nv12;
    return makeGpuFrameHandle(surface, std::move(rhi), std::move(meta));
}

#endif  // __APPLE__
```

Update the test body to call `importVtSurface(surface, m, rhi)` (not `importVtImageBuffer(surface->nativeHandle(), ...)`, since `nativeHandle()` is an IOSurface, not a CVImageBuffer). Add `vtkeepsurfaceimporter_apple.mm` to the Apple `target_sources` and `olr_test_gpu`.

- [ ] **Step 5: Run the test, expect PASS (or SKIP under offscreen)**

```sh
cmake --build build/c --target tst_gpuframedata && ctest --test-dir build/c -R tst_gpuframedata --output-on-failure
```
Expected on a GPU Mac: PASS (4 tests). Verify the decoder change did not regress H.264 decode:

```sh
ctest --test-dir build/c -L unit --output-on-failure
```
Expected: full unit suite green (the keep-surface path is opt-in; `decode` is unchanged).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add recorder_engine/ingest/nativevideodecoder.h recorder_engine/ingest/nativevideodecoder_videotoolbox.mm \
        playback/gpu/vtkeepsurfaceimporter.h playback/gpu/vtkeepsurfaceimporter_apple.mm \
        tests/unit/tst_gpuframedata.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(gpu-abstraction): VideoToolbox IOSurface keep-surface import -> GPU FrameHandle"
```

---

## Task 8: Wire the macOS slice into the worker + FrameProvider (single feed, behind OLR_GPU_PIPELINE)

**Precondition:** P0.1 (keep-surface import). The whole branch is gated on `gpuPipelineEnabled()` AND a non-null `GpuRhiContext`; any failure degrades to the existing CPU path.

**Files:**
- Modify: `playback/playbackworker.h` (add `m_gpuRhi`, `m_decodeFence`, `gpuReadToCpuCount` counter), `playback/playbackworker.cpp` (the H.264 keep-surface branch in `decodePacketIntoBank`, ~lines 557-641)
- Modify: `playback/frameprovider.h` / `.cpp` (a `FrameHandle` delivery path doing the single `readToCpu`; screenshot CPU-`QImage` path; GUI-thread affinity)
- Test: covered by the e2e harness (Task 9) + a worker-level unit slot in `tst_gpuframedata.cpp` if a seam is reachable; otherwise the e2e is the gate.

**Interfaces:**
- Consumes: `gpuPipelineEnabled` (Task 2), `GpuRhiContext::create` (Task 4), `decodeKeepSurface` + `importVtImageBuffer` (Task 7), `DecodeDoneFence::create` (Task 6), `FrameHandle`/`MediaVideoFrameView` (Phase 1).
- Produces: `PlaybackCounters::gpuReadToCpuCount` (qint64); `FrameProvider::deliverHandle(const FrameHandle&)`.

- [ ] **Step 1: Add the counter + RHI context to the worker (write the failing harness assertion first — see Task 9)**

In `playback/playbackworker.h`, extend `PlaybackCounters` (after `stagingVideoFramesDecoded`, line 84):

```cpp
        // GPU-backed readToCpu() calls executed (telemetry-contract copy-on-GPU-
        // path detector). With OLR_GPU_PIPELINE=1 on a single-feed slice this must
        // be exactly 1 per unique rendered bus surface (one bus -> count tracks the
        // distinct presented frames, one download each). 0 when the flag is off.
        qint64 gpuReadToCpuCount = 0;
```

Add member fields (in the private section near `convertToMediaVideoFrame`, line 162):

```cpp
    // GPU-resident pipeline (Phase 2 macOS slice). Null unless OLR_GPU_PIPELINE is
    // set AND a Metal QRhi initialized; otherwise the CPU path is used.
    std::shared_ptr<GpuRhiContext> m_gpuRhi;
    std::shared_ptr<DecodeDoneFence> m_decodeFence;
```

Include the GPU headers in `playbackworker.cpp` (guard the heavy ones so non-GPU builds are unaffected):

```cpp
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpuframedata.h"
#ifdef __APPLE__
#include "playback/gpu/vtkeepsurfaceimporter.h"
#endif
```

In the worker's open/start path (where the decoder bank is built, near line 1614-1672 where `NativeVideoDecoder` is constructed for H.264), initialize the GPU context once if enabled:

```cpp
    if (gpuPipelineEnabled() && !m_gpuRhi) {
        m_gpuRhi = GpuRhiContext::create();  // null -> stay on the CPU path
        if (m_gpuRhi) m_decodeFence = DecodeDoneFence::create();
    }
```

- [ ] **Step 2: Branch the H.264 decode path on keep-surface**

In `decodePacketIntoBank` (the `if (track->nativeDecoder)` block, lines 558-640), when `m_gpuRhi` is non-null, call `decodeKeepSurface` instead of `decode`, and in the surface callback build a GPU-backed handle. The handle assembly mirrors the existing `handleFrame` (PTS, decimation, dedup, insert) but produces a `FrameHandle` from the surface. Sketch (Apple-only path; non-Apple keeps `decode`):

```cpp
#ifdef __APPLE__
            if (m_gpuRhi) {
                auto onSurface = [&](void* cvImageBuffer, qint64 /*pts90k*/) {
                    // Micro-stress (§8): an injected GPU-alloc failure makes the
                    // import return a null handle; degrade to the CPU path for
                    // this frame instead of crashing the decode loop.
                    int64_t framePtsMs = (pktPts != AV_NOPTS_VALUE)
                        ? av_rescale_q(pktPts, tb, {1, 1000})
                        : ((lastVideoPtsMs != INT64_MIN) ? lastVideoPtsMs + frameDurMs() : P);
                    FrameMetadata meta;
                    meta.key.feedIndex = track->feedIndex;
                    meta.key.ptsMs = framePtsMs;
                    meta.key.width = codecParams ? codecParams->width : 0;
                    meta.key.height = codecParams ? codecParams->height : 0;
                    FrameHandle handle =
                        gpuConsumeInjectedAllocFailure()
                            ? FrameHandle()
                            : importVtImageBuffer(cvImageBuffer, meta, m_gpuRhi);
                    if (m_decodeFence) m_decodeFence->signalDecodeDone();  // publish fence
                    if (handle.isNull()) return;  // degrade: caller's next frame retries
                    QMutexLocker bufferLocker(&m_bufferMutex);
                    if (!track->buffer.insert(framePtsMs, handle, cap, protectLo, protectHi))
                        m_counters.framesDropped++;
                    if (m_outputCache) m_outputCache->insertVideoFrame(handle);
                    m_counters.decodedVideoFrames++;
                    lastVideoPtsMs = framePtsMs;
                };
                track->nativeDecoder->decodeKeepSurface(unit, onSurface, nullptr);
                return lastVideoPtsMs;
            }
#endif
```

> The forced-tiny-budget knob (`gpuForcedPerTrackBudget()`) is read where `cap` is computed (`capFrames`) so the micro-stress can shrink the window to force eviction-while-render on a single feed. Apply it: `int cap = gpuForcedPerTrackBudget() > 0 ? gpuForcedPerTrackBudget() : capFrames(trackCount);` guarded by `gpuPipelineEnabled()`.

- [ ] **Step 3: Add the FrameProvider handle path (single readback + screenshot + GUI affinity)**

In `playback/frameprovider.h`, add:

```cpp
    // GPU-resident delivery: the worker hands a FrameHandle; FrameProvider does
    // the SINGLE lazy readToCpu (the only full-frame GPU->CPU download on this
    // bus) and converts to a QVideoFrame on its way to the GUI-thread QVideoSink.
    // Also refreshes m_lastImage for the screenshot path so latestImage() never
    // forces a second readback.
    void deliverHandle(const FrameHandle& handle);
```

In `playback/frameprovider.cpp`, implement `deliverHandle`: build a `MediaVideoFrameView v(handle)` (one `readToCpu(Yuv420p)`), convert to `QVideoFrame` via the same YUV420P layout as `QtPreviewSink::toQVideoFrame` (so the picture is byte-identical to the CPU path), cache the `QImage` for `latestImage()`, and `deliverFrame(qFrame)` (which already marshals to the GUI-thread `QVideoSink`). The screenshot path (`latestImage()`) returns the cached image — no second download.

> **GUI-thread affinity:** `deliverFrame` already crosses to the GUI thread via the existing `QVideoSink` mechanism; `deliverHandle` performs the `readToCpu` on the *worker* side (the readback runs on `GpuRhiContext`'s render thread, invoked synchronously from the worker), then hands a CPU `QVideoFrame` across — never a GPU surface across the thread boundary, honoring D11 texture affinity.

- [ ] **Step 4: Build + run the slice manually (smoke)**

```sh
cmake --build build/c --target play_harness record_harness
# CPU path unchanged:
OLR_GPU_PIPELINE=0 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness play1x 1 9301
# GPU path (on a GPU Mac):
OLR_GPU_PIPELINE=1 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness play1x 1 9302
```
Expected: both PASS; the `=1` run additionally reports `gpuReadToCpuCount>=1` (asserted in Task 9).

- [ ] **Step 5: Zero-regression + commit**

```sh
ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/playbackworker.h playback/playbackworker.cpp playback/frameprovider.h playback/frameprovider.cpp
git commit -m "feat(gpu-abstraction): macOS single-feed slice behind OLR_GPU_PIPELINE (decode->GPU handle->preview)"
```

---

## Task 9: Slice success-criteria gates in the playback e2e (copy-detector, placeholder/held deltas, dual-flag run)

**Precondition:** Task 8 wired the slice.

**Files:**
- Modify: `tests/e2e/play_harness.cpp` (emit `gpuReadToCpuCount` on the COUNTERS line; lines 148-150 / 606-608)
- Modify: `tests/e2e/run_playback_e2e.sh` (parse `gpuReadToCpuCount`; add `OLR_GPU_PIPELINE=1` assertions on `play1x`)

**Interfaces:**
- Consumes: `PlaybackCounters::gpuReadToCpuCount` (Task 8), the existing `placeholderFramesDelta` / `heldFramesDelta` / `decodedVideoFrames` counters (`run_playback_e2e.sh:240,243,249`).
- Produces: a `play1x` gate that, when run with `OLR_GPU_PIPELINE=1`, asserts correct picture (`decodedVideoFrames>0`), `placeholderFramesDelta==0`, `heldFramesDelta==0`, and `gpuReadToCpuCount>=1` (one download per unique rendered bus surface; single bus). With `OLR_GPU_PIPELINE=0` it asserts `gpuReadToCpuCount==0`.

- [ ] **Step 1: Emit the new counter from the harness**

In `tests/e2e/play_harness.cpp`, add `gpuReadToCpuCount=%lld` to both `printf("COUNTERS ...")` format strings (lines 148 and 606) and pass `c.gpuReadToCpuCount`. Match the existing `%lld` style for the qint64 deltas.

- [ ] **Step 2: Write the failing gate in the driver**

In `tests/e2e/run_playback_e2e.sh`, after the existing `get` extractions (line ~249), add:

```sh
gpuReadToCpuCount="$(get gpuReadToCpuCount)"
[ -n "$gpuReadToCpuCount" ] || gpuReadToCpuCount="?"
```

In the `play1x)` scenario block (line 284), add the GPU-flag-aware assertions:

```sh
    play1x)
        # ... existing play1x assertions ...
        if [ "${OLR_GPU_PIPELINE:-0}" = "1" ]; then
            # GPU vertical slice (§8): correct picture, no gray flash / stalls, and
            # exactly one GPU->CPU download per unique rendered bus surface (single
            # feed = single bus). copy-detector: gpuReadToCpuCount must be >=1 and
            # must NOT exceed decodedVideoFrames (no redundant downloads).
            if ! num "$placeholderFramesDelta" || [ "$placeholderFramesDelta" -ne 0 ]; then
                echo "FAIL: GPU slice painted gray (placeholderFramesDelta=$placeholderFramesDelta, expected 0)"; exit 1
            fi
            if ! num "$heldFramesDelta" || [ "$heldFramesDelta" -ne 0 ]; then
                echo "FAIL: GPU slice stalled (heldFramesDelta=$heldFramesDelta, expected 0)"; exit 1
            fi
            if ! num "$gpuReadToCpuCount" || [ "$gpuReadToCpuCount" -lt 1 ]; then
                echo "FAIL: GPU slice did no GPU readback (gpuReadToCpuCount=$gpuReadToCpuCount, expected >=1)"; exit 1
            fi
            if ! num "$decodedVideoFrames" || [ "$gpuReadToCpuCount" -gt "$decodedVideoFrames" ]; then
                echo "FAIL: GPU slice over-downloaded (gpuReadToCpuCount=$gpuReadToCpuCount > decodedVideoFrames=$decodedVideoFrames)"; exit 1
            fi
        else
            if num "$gpuReadToCpuCount" && [ "$gpuReadToCpuCount" -ne 0 ]; then
                echo "FAIL: CPU path performed a GPU readback (gpuReadToCpuCount=$gpuReadToCpuCount, expected 0)"; exit 1
            fi
        fi
        ;;
```

- [ ] **Step 3: Run both flag states**

```sh
cmake --build build/c --target play_harness record_harness
OLR_GPU_PIPELINE=0 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness play1x 1 9303
OLR_GPU_PIPELINE=1 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness play1x 1 9304
```
Expected: `=0` PASS with `gpuReadToCpuCount=0`; `=1` PASS with `gpuReadToCpuCount>=1`, `placeholderFramesDelta==0`, `heldFramesDelta==0`. On a host with no Metal device, the `=1` run degrades to CPU and `gpuReadToCpuCount==0` — document that the `=1` gate is GPU-host-only (mirrors §9 "macOS CI stays CPU-oracle-only").

- [ ] **Step 4: Commit**

```sh
git add tests/e2e/play_harness.cpp tests/e2e/run_playback_e2e.sh
git commit -m "test(gpu-abstraction): play1x copy-detector + placeholder/held gates under OLR_GPU_PIPELINE"
```

---

## Task 10: Early micro-stress — evict-while-render + OOM-degrade with the decode-done fence

**Precondition:** Task 6 (fence), Task 8 (slice). This is the §8 micro-stress: the two highest-rated program risks get an early signal before `gpu-sync`/`gpu-compositor`.

**Files:**
- Create: `tests/unit/tst_gpu_microstress.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `makeAppleNv12Surface`, `gpuSetInjectedAllocFailures`/`gpuConsumeInjectedAllocFailure`, `GpuRhiContext`, `GpuFrameData`/`makeGpuFrameHandle`, `DecodeDoneFence` (all prior tasks).
- Produces: two gates — (a) concurrent evict-while-render does not read a freed surface (the surface stays alive while a render/readback fence is in flight, §4 lifetime contract); (b) injected GPU-OOM degrades to a CPU handle without crashing.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpu_microstress.cpp`:

```cpp
// The Phase-2 early micro-stress (§8): on a single feed, exercise the two
// highest-rated program risks before gpu-sync/gpu-compositor land —
//  (a) concurrent evict-while-render must NOT free a surface a readback is using
//      (the §4 surface-lifetime contract: the handle retains the surface until
//      fences retire), and
//  (b) injected GPU-OOM degrades to a CPU handle without crashing the loop.
#include <QtTest>

#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/output/framehandle.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#endif

#include <atomic>
#include <thread>

class TestGpuMicrostress : public QObject {
    Q_OBJECT
private slots:
#ifdef __APPLE__
    void evictWhileRenderDoesNotFreeInUseSurface();
#endif
    void injectedOomDegradesToCpuHandle();
};

#ifdef __APPLE__
void TestGpuMicrostress::evictWhileRenderDoesNotFreeInUseSurface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto surface = makeAppleNv12Surface(64, 48);
    FrameMetadata m;
    m.key.format = FramePixelFormat::Nv12;
    m.key.width = 64; m.key.height = 48;
    FrameHandle held = makeGpuFrameHandle(surface, rhi, m);

    auto fence = DecodeDoneFence::create();
    QVERIFY(fence != nullptr);

    std::atomic<bool> readbackOk{false};
    // "Render/readback" thread: waits for decode-done, then reads the surface.
    std::thread consumer([&] {
        QVERIFY(fence->waitDecodeDone(2000));
        const CpuPlanes p = held.readToCpu(FramePixelFormat::Yuv420p);  // surface still alive
        readbackOk = p.isValid();
    });
    // "Eviction" thread: drops its own ref to the handle. The shared_ptr keeps
    // the surface alive while `held` (and the consumer's use) outlive it.
    std::thread evictor([&] {
        FrameHandle evicting = held;  // a second ref (cache alias)
        fence->signalDecodeDone();    // publish: consumer may now read
        evicting = FrameHandle();     // "evict" this ref — must NOT free the surface
    });
    evictor.join();
    consumer.join();
    QVERIFY(readbackOk);              // readback saw live pixels, never a freed surface
    QVERIFY(held.data()->gpuSurface() != nullptr);
}
#endif

void TestGpuMicrostress::injectedOomDegradesToCpuHandle() {
    // The slice's degrade contract: when GPU surface alloc fails (injected), the
    // path falls back to a CPU handle rather than crashing. Here we model the
    // worker's branch: inject a failure, attempt the GPU surface, and on null
    // build a CPU handle instead.
    gpuSetInjectedAllocFailures(1);
    FrameHandle handle;
#ifdef __APPLE__
    auto surface = makeAppleNv12Surface(64, 48);  // consumes the injected failure -> null
    if (!surface) handle = solidYuv420pHandle(64, 48, 16, 128, 128);  // CPU degrade
#else
    if (gpuConsumeInjectedAllocFailure()) handle = solidYuv420pHandle(64, 48, 16, 128, 128);
#endif
    QVERIFY(!handle.isNull());
    QVERIFY(!handle.isGpuBacked());                 // degraded to CPU
    const CpuPlanes p = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(p.isValid());
}

QTEST_GUILESS_MAIN(TestGpuMicrostress)
#include "tst_gpu_microstress.moc"
```

Register in `tests/unit/CMakeLists.txt`:

```cmake
olr_add_unit_test(tst_gpu_microstress olr_test_playback)
if(APPLE)
    target_link_libraries(tst_gpu_microstress PRIVATE olr_test_gpu)
endif()
```

- [ ] **Step 2: Run the test, expect PASS (the implementation already exists from Tasks 3-8)**

```sh
cmake --build build/c --target tst_gpu_microstress && ctest --test-dir build/c -R tst_gpu_microstress --output-on-failure
```
Expected: on a GPU Mac, PASS (2 tests) — the evict-while-render gate proves the surface outlives the in-flight readback (refcount + §4 lifetime), and the OOM-degrade gate proves a null GPU surface falls back to a valid CPU handle. The OOM test runs on all platforms (no RHI needed). Under offscreen, the evict test SKIPs.

> If the evict test FAILS (readback saw a freed surface), that is the real evict-while-render race surfacing early — exactly the §8 signal. Stop and apply the §4 lifetime fix (the handle must hold the surface ref past the fence) before proceeding; do NOT weaken the gate.

- [ ] **Step 3: Run under ASan/UBSan (no new findings, §8 criterion)**

```sh
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/asan --target tst_gpu_microstress tst_gpuframedata tst_gpurhicontext
ctest --test-dir build/asan -R "tst_gpu_microstress|tst_gpuframedata|tst_gpurhicontext" --output-on-failure
```
Expected: PASS with no ASan/UBSan findings (use-after-free, data race on the surface refcount, leaked `CVPixelBuffer`). This is the §8 "no new ASan/UBSan findings" gate for the GPU edge.

- [ ] **Step 4: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add tests/unit/tst_gpu_microstress.cpp tests/unit/CMakeLists.txt
git commit -m "test(gpu-abstraction): early micro-stress (evict-while-render + OOM-degrade) with decode-done fence"
```

---

## Task 11: Independent concurrency review + finalize (worker-threading gate, CLAUDE.md)

**Precondition:** all prior tasks merged-locally; the slice builds and both flag states pass.

**Files:** none new — this task is the verification + review gate the project mandates for worker-threading changes.

- [ ] **Step 1: Full local pre-flight**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
OLR_GPU_PIPELINE=0 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness play1x 1 9311
OLR_GPU_PIPELINE=0 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness seekplay 1 9312
OLR_GPU_PIPELINE=1 tests/e2e/run_playback_e2e.sh build/c/tests/e2e/play_harness build/c/tests/e2e/record_harness play1x 1 9313
```
Expected: unit suite green; all three e2e PASS. The `=0` runs prove zero regression; the `=1` run proves the slice.

- [ ] **Step 2: Request an independent concurrency review**

Per CLAUDE.md and §9, the playback worker's threading and the `gpu-sync` fence get an independent review before merge. Use `superpowers:requesting-code-review`, focusing the reviewer on: (a) the `GpuRhiContext` render-thread handoff (D11 texture affinity — no GPU surface crosses to the GUI thread; the readback runs on the render thread and only a CPU `QVideoFrame` is marshalled), (b) the §4 surface-lifetime contract (the handle holds the `CVPixelBuffer` ref past the in-flight readback; eviction never frees a fenced surface), and (c) the `m_bufferMutex` discipline in the keep-surface branch (insert under the lock, never fence-wait under the mutex — the future `gpu-sync` lock rule).

- [ ] **Step 3: Address review, re-run the gates, then open the PR**

```sh
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin gpu-phase2-gpu-abstraction
gh pr create --title "GPU Phase 2: gpu-abstraction + macOS vertical slice" --body "..."
```
PR body: summarize the slice, the success criteria met (`OLR_GPU_PIPELINE=0` suite green; `=1` correct picture + `placeholderFramesDelta==0` + `heldFramesDelta==0` + copy-detector==1 per bus + ±1-LSB readback + no ASan/UBSan), and call out which Phase-0 preconditions held vs. branched. End the body with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.

- [ ] **Step 4: Confirm CI green + merge landed**

```sh
gh pr checks --watch
# after merge:
git fetch origin && git merge-base --is-ancestor <merge-sha> origin/main && echo "landed on main"
```
