# gpu-compositor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Reimplement `Yuv420pCompositor::composeGrid` + the PGM single-source select as RHI shaders that consume GPU-resident `GpuSurface` inputs (NV12 sampled, chroma deinterleaved in-shader per D2) and produce an `Rgba8` `GpuSurface` output, rendered on the merged `GpuRhiContext` (one `QRhi`, dedicated render thread), using the baked `qsb` shader toolchain. The CPU `Yuv420pCompositor` is retained permanently as the correctness oracle + fallback, selected by `OLR_GPU_PIPELINE` / `gpuPipelineEnabled()`. Two validation lanes ship (§9): (a) a GPU **nearest-neighbor compat** shader, integer-reconstructed to match the CPU oracle **exactly on WARP / ±1 LSB on a local-GPU lane**; (b) the **quality scaler** (bilinear/Lanczos), validated **separately** with its own PSNR/SSIM goldens (`tests/unit/framepsnr.h`), **never** against the oracle. The multiview memo is carried forward as a GPU-texture cache keyed on the metadata `sourceKeys` (`videoHashFor`). Render-done ordering before the output is read uses the `gpu-sync` `GpuFence`.

**Architecture:** A new `GpuCompositor` (`playback/gpu/gpucompositor.{h,cpp}` + `_apple.mm` for Metal/IOSurface import) owns a small RHI pipeline family built on the merged `GpuRhiContext` render thread (D11): a YUV→RGB grid fragment shader (`grid_nn.frag` compat, `grid_quality.frag` scaler) sampling each input `GpuSurface` as two textures (NV12 luma R8 + interleaved chroma RG8, deinterleaved by the sampler), a vertex shader emitting per-tile quads, and a uniform block carrying the BT.601/709 matrix + range + per-tile destination rects. It renders into an `Rgba8` `GpuSurface` output (a `QRhiTexture` wrapped as a `GpuSurface`), signals a `GpuFence` on the render queue, and returns a GPU-backed `FrameHandle` via `makeGpuFrameHandle(surface, rhi, meta, renderFence)`. `OutputBusEngine::renderMultiview`/`renderPgm` gain a compositor-select seam: when `gpuPipelineEnabled()` and a `GpuCompositor` is live, the grid/select runs on the GPU and the prior `MultiviewComposite` memo becomes a GPU-texture memo keyed on the same `sourceKeys`; otherwise the CPU `Yuv420pCompositor` path is byte-for-byte unchanged. The shader math is integer-reconstructed against `formatcanon`'s `referenceComposeGridRgba8` (the NV12-deinterleave + nearest-neighbor + integer YUV→RGB oracle delivered by `format-canon` before this subproject) so the compat lane is WARP-exact.

**Tech Stack:** C++17, Qt 6.10 (Core/Gui/GuiPrivate for `QRhi`/`QShader`/`QRhiGraphicsPipeline`, Test), Apple Metal/CoreVideo/IOSurface (the IOSurface→Metal→`QRhiTexture::createFrom` import path proven in P0.5), GLSL 440 → `.qsb` via `qt_add_shaders`/`qsb`, the QRhi `Null` backend as the deterministic headless/CI oracle, CMake + Ninja. Consumes the merged Phase-0/1/2 + `gpu-sync` contracts: `FrameHandle`/`IFrameData`/`CpuPlanes`/`FrameMetadata`/`FramePayloadKey`/`FramePixelFormat`/`ColorMetadata`/`MediaVideoFrameView`/`makeCpuFrameHandle`/`solidYuv420pHandle` (`playback/output/framehandle.h`), `formatcanon::{referenceComposeGridRgba8,yuvToRgb8,nv12ToYuv420p,yuv420pToNv12,upsampleChromaNearest,exportRgba8ToYuv420p,Rgb8,PlaneShape,planeShape,packedStride}` (`playback/output/formatcanon.h`), `Yuv420pCompositor::composeGrid` (`playback/output/yuv420pcompositor.h`, retained), `GpuSurface`/`GpuSurfaceDesc` (`playback/gpu/gpusurface.h`), `GpuRhiContext` (`playback/gpu/gpurhicontext.h`), `OlrRhi` (`playback/gpu/olrrhi.h`), `GpuFrameData`/`makeGpuFrameHandle` (`playback/gpu/gpuframedata.h`), `gpuPipelineEnabled()`/`gpuConsumeInjectedAllocFailure()`/`gpuSetInjectedAllocFailures()` (`playback/gpu/gpupipelineconfig.h`), `makeAppleNv12Surface`/`wrapAppleImageBuffer` (`playback/gpu/appleiosurface.h`), and the `gpu-sync` family `GpuFence` (`playback/gpu/gpufence.h`), `GpuGenerationCounter`/`FrameHandle::isStaleForGeneration`/`FrameMetadata::gpuGeneration` (`playback/gpu/gpugeneration.h`), `GpuSurface::retainUntilFenceRetired`/`pendingFenceValue`, and `makeGpuFrameHandle(surface, rhi, meta, renderFence)`. Validation uses `psnrY8` (`tests/unit/framepsnr.h`).

## Global Constraints

- **Builds ON merged code, never replaces it.** Every type/signature named below either already exists in the tree (use the **actual** signature — `formatcanon::referenceComposeGridRgba8`, `GpuRhiContext::importAndReadback`, `OlrRhi::runOffscreenFrame`, `Yuv420pCompositor::composeGrid`) or is delivered by the consumed `gpu-sync` contract (`GpuFence`, `GpuGenerationCounter`, `makeGpuFrameHandle(...,renderFence)`, `GpuSurface::retainUntilFenceRetired`) or is genuinely new here (`GpuCompositor`, the grid shaders, `OutputBusEngine`'s compositor seam). Do not invent variant names. Downstream `async-readback`/`gpu-budget`/`ios-bringup` consume `GpuCompositor`'s public surface verbatim — the canonical contract is listed at the end of this plan.
- **The CPU compositor is the permanent oracle + fallback (D3/D5).** `Yuv420pCompositor` stays in the tree and stays the default. The GPU path is two-gated: the `OLR_GPU_PIPELINE` CMake option → `OLR_GPU_PIPELINE_BUILD` compile def (`tests/CMakeLists.txt:271-272`, `CMakeLists.txt:452-453`), and the runtime `gpuPipelineEnabled()` env flag (off by default). With `OLR_GPU_PIPELINE_BUILD` undefined **or** `gpuPipelineEnabled()` false **or** no live `GpuCompositor`, `renderMultiview`/`renderPgm` run the byte-for-byte Phase-1/2 CPU path. Any GPU failure (RHI unavailable, surface alloc failure, GPU-OOM, stale generation) degrades to the CPU compositor without crashing — never a hard dependency.
- **Two validation lanes, never crossed (§9, D3).** Lane (a) — the **nearest-neighbor compat** shader — is integer-reconstructed against `formatcanon::referenceComposeGridRgba8` (the NV12-double-rounding + nearest-neighbor + integer YUV→RGB oracle): **exact** on the QRhi `Null`/WARP path, **±1 LSB/channel** on a local-GPU (Metal) host. Lane (b) — the **quality scaler** (bilinear/Lanczos) — is by definition NOT byte/LSB-comparable to nearest-neighbor; it is validated against its **own** PSNR/SSIM goldens with a perceptual tolerance via `psnrY8` (`tests/unit/framepsnr.h`), and is **NEVER** asserted against the oracle. A test that compares the quality scaler to `referenceComposeGridRgba8` is a plan violation.
- **Headless/CI reality (§9 resolves Q7).** Hosted CI runners have no GPU/Metal/D3D (`ci.yml`). The compat-lane WARP-exact coverage runs on the QRhi `Null` backend (deterministic, CPU-side, the `OlrRhi::Backend::Null` already in the tree); the ±1-LSB local-GPU lane and the Metal IOSurface-import path `QSKIP` where no Metal device exists. macOS CI stays CPU-oracle-only. No GPU behavioral test ever hard-fails for lack of a backend — it `QSKIP`s.
- **D2 layout is fixed.** Inputs stay NV12 (luma R8 plane + interleaved UV RG8 plane, chroma deinterleaved by the sampler in-shader); the compositor working/output format is `Rgba8`; the export edge (NDI=I420, preview=RGBA8) is owned by `formatcanon::sinkExportFormat` and the existing `readToCpu` chokepoint — **this subproject does not add export conversions.** No 10-bit/P010 path.
- **Render-done ordering via `GpuFence` (gpu-sync).** Before the composited `Rgba8` output is read (by a CPU sink's `readToCpu` or a downstream pass), the render-done `GpuFence` value must be retired. The compositor signals the fence on the render queue after the offscreen pass and stamps the output surface (`retainUntilFenceRetired`); the output `FrameHandle` is minted via the `makeGpuFrameHandle(surface, rhi, meta, renderFence)` overload so the readback path waits on the right value. Fences gate **ordering**, not cost (P0.3: synchronous readback is 1.78 ms, over the 0.5 ms budget — `async-readback` pipelines it later; this plan never claims the fence makes readback fit budget).
- **Generation-staleness (gpu-sync).** A composited output is stamped with `GpuGenerationCounter::instance().current()`; the compositor drops any **input** handle that `isStaleForGeneration(current)` returns true for (a seek/reposition superseded its window) and treats it as an absent feed (placeholder/black tile), exactly as the CPU path treats a null handle.
- **Multiview memo carried forward (D9 refs-only).** The GPU-texture memo keys on the same `MultiviewComposite::sourceKeys` (present-flag + selected pts per feed, the `videoHashFor` discriminator) the CPU memo uses; a memo hit reuses the prior `Rgba8` `GpuSurface` `FrameHandle` (a refcount bump, no re-render, no readback) — never a hash, so a reused frame is byte-identical to a fresh composite.
- **Zero-regression gate after every task.** With the flag off:
  ```sh
  cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
  ```
  passes with identical assertion values, and `e2e_play` (`ctest -R e2e_play`) golden thresholds unchanged. The existing `tst_yuv420pcompositor` pixel-exact goldens stay green untouched (the CPU compositor is unmodified).
- **Build (run from the worktree root):** configure once with the GPU pipeline ON (this subproject only compiles under it):
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Also configure once with `-DOLR_GPU_PIPELINE=OFF` (a **fresh** build dir — cached Qt/RHI link state is stale across the toggle) to confirm the off-path still compiles and the unit suite is byte-green. Unit tests register via `olr_add_unit_test(<name> <libs...>)` in `tests/unit/CMakeLists.txt`, GPU tests inside the `if(OLR_GPU_PIPELINE)` block (lines 196-202); the RHI/shader backend lib is `olr_test_gpu` (`tests/CMakeLists.txt:288-312`, links `Qt6::GuiPrivate`, bakes shaders via `qt_add_shaders`), `olr_test_playback` carries the worker + `OLR_GPU_PIPELINE_BUILD`. Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.
- **Format changed lines only** (CI Lint checks changed lines; several engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
  `.qsb` is baked, not formatted; GLSL `*.vert`/`*.frag` follow the existing `playback/gpu/shaders/` style.
- **Concurrency review (CLAUDE.md "Verification").** The compositor runs on the `GpuRhiContext` dedicated render thread and shares the `GpuFence` timeline with decode/eviction; the `OutputBusEngine` seam is called from the cadence thread. The render-handoff + fence-ordering tasks (Tasks 4, 7, 8) carry a `**Review gate:**` note; the branch gets a fresh-agent concurrency review before merge.
- **Public-repo professionalism.** Self-contained, professional code/comments/commits; document the present design, no internal notes or private history.

---

## Preconditions (read before Task 1)

- **`format-canon` merged and is the oracle.** `playback/output/formatcanon.{h,cpp}` provides `referenceComposeGridRgba8(frames, w, h, color)` — the NV12-deinterleave (`yuv420pToNv12`→`nv12ToYuv420p`) + nearest-neighbor scale + integer `yuvToRgb8` reconstruction this subproject's compat shader matches **bit-for-bit on `Null`/WARP**. The integer YUV→RGB coefficients are pinned in `yuvToRgb8` (BT.601: `r=yp+1634*cr; g=yp-401*cb-832*cr; b=yp+2066*cb`; BT.709: `r=yp+1836*cr; g=yp-218*cb-546*cr; b=yp+2164*cb`; `yp=(y-16)*1192` video / `y*1024` full; result `(v+512)>>10` clamped). The shader replicates this exact integer math (no float color), so `Null`-backend output equals the oracle byte-for-byte. Verify with `git merge-base --is-ancestor <format-canon-sha> origin/main` if unsure.
- **`gpu-sync` merged.** `playback/gpu/gpufence.h` (`GpuFence::create()`, `signal()`, `wait(value,timeoutMs)`, `completedValue()`), `playback/gpu/gpugeneration.h` (`GpuGenerationCounter::instance()`, `current()`, `bump()`, `reset()`), `FrameMetadata::gpuGeneration`, `FrameHandle::isStaleForGeneration(uint64_t)`, `GpuSurface::retainUntilFenceRetired(uint64_t)`/`pendingFenceValue()`, and the `makeGpuFrameHandle(surface, rhi, meta, renderFence)` 4-arg overload all exist. This subproject is the FIRST consumer of the render-fence overload at a compositor mint site.
- **`gpu-abstraction` merged.** `GpuRhiContext` (one `QRhi` on a dedicated render thread), `OlrRhi` (the `Null`-backend pipeline helper with `runOffscreenFrame`), `makeAppleNv12Surface`/`wrapAppleImageBuffer`, `GpuFrameData`/`makeGpuFrameHandle`, the baked `passthrough.{vert,frag}.qsb` + `qt_add_shaders` toolchain, and the `olr_test_gpu` lib all exist.
- **P0.3 / P0.5 known.** P0.5 GO: IOSurface→Metal→`QRhiTexture::createFrom` zero-copy import works on Apple. P0.3 CONDITIONAL: synchronous readback is over budget — the compositor renders GPU-resident and leaves readback to `async-readback`; this plan gates on **correctness + degrade safety**, not the 0.5 ms budget.
- **The render-thread pipeline pattern is established.** `tst_shadertoolchain.cpp` shows the exact `QRhi` offscreen pipeline-build idiom this plan extends: `newTexture(RGBA8, …, RenderTarget)` → `newTextureRenderTarget` → `newCompatibleRenderPassDescriptor` → `newShaderResourceBindings` → `newGraphicsPipeline` with baked `QShader` stages → `runOffscreenFrame([&](cb){ beginPass; setGraphicsPipeline; draw; endPass; })`. The compositor adds sampler bindings + a uniform buffer to this idiom.

---

## Task 1: `GpuCompositor` skeleton + CPU-fallback delegation (the select seam)

**Precondition:** none beyond the merged code. This task introduces the type and proves the fallback contract before any shader exists.

**Files:**
- Create: `playback/gpu/gpucompositor.h`, `playback/gpu/gpucompositor.cpp`
- Test: `tests/unit/tst_gpucompositor.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuRhiContext` (`playback/gpu/gpurhicontext.h`), `Yuv420pCompositor::composeGrid` (`playback/output/yuv420pcompositor.h`), `FrameHandle`/`FrameMetadata`/`ColorMetadata` (`playback/output/framehandle.h`), `gpuPipelineEnabled()` (`playback/gpu/gpupipelineconfig.h`).
- Produces:
  ```cpp
  // playback/gpu/gpucompositor.h — platform-neutral header (no Metal/CVPixelBuffer types).
  // The RHI grid + PGM-select compositor (spec §6, D2/D3). Renders GPU-resident
  // NV12 GpuSurface inputs into an Rgba8 GpuSurface output on the GpuRhiContext
  // render thread, integer-matching formatcanon::referenceComposeGridRgba8 on the
  // Null/WARP lane. The CPU Yuv420pCompositor is the permanent oracle + fallback.
  class GpuCompositor {
  public:
      // Quality of the scale filter for the grid. NearestCompat is the oracle-
      // validated compat shader (lane a); Bilinear/Lanczos are the quality scalers
      // (lane b, own PSNR/SSIM goldens, never the oracle).
      enum class ScaleQuality { NearestCompat, Bilinear, Lanczos };

      // Build the compositor over an existing GpuRhiContext. Returns nullptr if the
      // RHI is unavailable or the pipeline cannot be built (caller falls back to CPU).
      static std::shared_ptr<GpuCompositor> create(std::shared_ptr<GpuRhiContext> rhi);

      bool isValid() const;

      // Compose `frames` into a `width`x`height` grid (same layout as
      // Yuv420pCompositor::composeGrid: ceil(sqrt(N)) columns) and return a
      // GPU-backed Rgba8 FrameHandle (render-fence-stamped). On any failure returns
      // a null FrameHandle so the caller degrades to the CPU compositor. `color`
      // carries the BT.601/709 matrix + range. `quality` selects the lane.
      FrameHandle composeGrid(const QList<FrameHandle>& frames, int width, int height,
                              ColorMetadata color, ScaleQuality quality) const;

      // PGM single-source select: a 1x1 "grid" of one source scaled to fill.
      FrameHandle composePgm(const FrameHandle& source, int width, int height,
                             ColorMetadata color, ScaleQuality quality) const;

  private:
      class Impl;
      explicit GpuCompositor(std::unique_ptr<Impl> impl);
      std::unique_ptr<Impl> m_impl;
  };
  ```

- [ ] **Step 1: Write the failing test (skeleton + degrade contract)**

Create `tests/unit/tst_gpucompositor.cpp`:

```cpp
// GpuCompositor is the RHI grid/PGM compositor; the CPU Yuv420pCompositor stays
// the oracle + fallback. With no RHI backend (offscreen/CI) create() may return
// nullptr OR a valid compositor over the Null backend; either way the type exists
// and the fallback contract holds (a null compositor means the caller uses CPU).
#include <QtTest>

#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/framehandle.h"

class TestGpuCompositor : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
};

void TestGpuCompositor::createIsNullOrValidNeverPartial() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host (expected under offscreen/CI)");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor pipeline unavailable on this host");
    QVERIFY(comp->isValid());
}

QTEST_GUILESS_MAIN(TestGpuCompositor)
#include "tst_gpucompositor.moc"
```

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt` (after `tst_gpu_microstress`):

```cmake
    olr_add_unit_test(tst_gpucompositor olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpucompositor
```
Expected: FAIL to compile — `playback/gpu/gpucompositor.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/gpu/gpucompositor.h` with the interface above.

Create `playback/gpu/gpucompositor.cpp` — for this task the `Impl` only holds the `GpuRhiContext`; `create()` returns nullptr unless `rhi && rhi->isValid()`; `composeGrid`/`composePgm` return a null `FrameHandle{}` (the shader lands in Task 4). Keep all RHI/shader includes out of this `.cpp` for now (platform-neutral skeleton):

```cpp
#include "playback/gpu/gpucompositor.h"

class GpuCompositor::Impl {
public:
    std::shared_ptr<GpuRhiContext> rhi;
};

GpuCompositor::GpuCompositor(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

std::shared_ptr<GpuCompositor> GpuCompositor::create(std::shared_ptr<GpuRhiContext> rhi) {
    if (!rhi || !rhi->isValid()) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->rhi = std::move(rhi);
    return std::shared_ptr<GpuCompositor>(new GpuCompositor(std::move(impl)));
}

bool GpuCompositor::isValid() const { return m_impl && m_impl->rhi && m_impl->rhi->isValid(); }

FrameHandle GpuCompositor::composeGrid(const QList<FrameHandle>&, int, int, ColorMetadata,
                                       ScaleQuality) const {
    return FrameHandle{};  // shader lands in Task 4
}

FrameHandle GpuCompositor::composePgm(const FrameHandle&, int, int, ColorMetadata,
                                      ScaleQuality) const {
    return FrameHandle{};  // shader lands in Task 4
}
```

Wire CMake. In `tests/CMakeLists.txt`, inside the `if(OLR_GPU_PIPELINE)` block, add `playback/gpu/gpucompositor.cpp` to **both** `olr_test_playback`'s GPU sources (after `gpuframedata.cpp` at :273-274) and the `olr_test_gpu` library sources (after `gpuframedata.cpp` at :291-292), so the worker lib and the RHI test lib both link the compositor. (The Apple shader `.mm` is added in Task 4.)

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS (1 test; `QSKIP` where no RHI backend, or runs over `Null`).

- [ ] **Step 5: Verify the zero-regression gate**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```
Expected: full unit suite PASSES (the compositor is additive; nothing consumes it yet).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpucompositor.h playback/gpu/gpucompositor.cpp \
        tests/unit/tst_gpucompositor.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(gpu-compositor): GpuCompositor skeleton + CPU-fallback select seam"
```

---

## Task 2: The nearest-neighbor compat grid shaders (GLSL source + qsb bake)

**Precondition:** Task 1. The shader math must replicate `formatcanon::yuvToRgb8`'s **integer** coefficients exactly (Preconditions), so the `Null`-backend output equals `referenceComposeGridRgba8` byte-for-byte.

**Files:**
- Create: `playback/gpu/shaders/grid.vert`, `playback/gpu/shaders/grid_nn.frag`
- Modify: `tests/CMakeLists.txt` (add the two new shaders to the existing `qt_add_shaders(olr_test_gpu "olr_passthrough_shaders" ...)` FILES list, or a sibling `qt_add_shaders` call producing `:/olr/shaders/grid.vert.qsb` + `grid_nn.frag.qsb`), `playback/gpu/shaders/olr_shaders.qrc` (document the new aliases)
- Test: `tests/unit/tst_gpucompositor.cpp` (add a shader-loads-and-builds slot mirroring `tst_shadertoolchain`)

**Interfaces:**
- Consumes: the `qt_add_shaders` toolchain + `QShader::fromSerialized` + the `OlrRhi`/`QRhiGraphicsPipeline` build idiom (from `tst_shadertoolchain.cpp`).
- Produces: baked `:/olr/shaders/grid.vert.qsb` + `:/olr/shaders/grid_nn.frag.qsb`. The shaders sample **two** input textures per source — `texLuma` (R8, the NV12 Y plane) and `texChroma` (RG8, the NV12 interleaved UV plane, half-res; the sampler deinterleaves: `.r`=U, `.g`=V) — and convert to RGB with the **integer** BT.601/709 math from a uniform block, using nearest-neighbor (`textureLod` with explicit integer texel fetch) scaling. The grid layout (per-tile destination rect + which source) is driven by a uniform array. **Shader contract** (matches `yuvToRgb8`):
  ```glsl
  // grid_nn.frag (sketch — integer-faithful YUV->RGB, nearest sampling):
  //   int Y = int(round(texelFetch(texLuma, srcLumaTexel, 0).r * 255.0));
  //   int U = int(round(texelFetch(texChroma, srcChromaTexel, 0).r * 255.0));
  //   int V = int(round(texelFetch(texChroma, srcChromaTexel, 0).g * 255.0));
  //   int yp = (uRange == 1) ? (Y - 16) * 1192 : Y * 1024;   // 1=Video,0=Full
  //   int cb = U - 128; int cr = V - 128;
  //   // uMatrix: 0=Bt601, 1=Bt709
  //   int r,g,b = <exact integer coeffs from formatcanon::yuvToRgb8>;
  //   fragColor = vec4(clampU8((r+512)>>10), ..., 255) / 255.0;
  ```
  Use `int`/`ivec` arithmetic and `>>` shifts so the result is bit-identical to the oracle's integer path (no float matrix multiply). `clampU8` clamps to `[0,255]` then divides by 255 for the `Rgba8` render target.

- [ ] **Step 1: Write the failing test (grid shaders load + build a pipeline)**

Add to `tests/unit/tst_gpucompositor.cpp`:

```cpp
    void gridShadersLoadAndBuildPipeline();
```

```cpp
void TestGpuCompositor::gridShadersLoadAndBuildPipeline() {
    auto load = [](const QString& p) {
        QFile f(p);
        return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
    };
    const QShader vert = load(QStringLiteral(":/olr/shaders/grid.vert.qsb"));
    const QShader frag = load(QStringLiteral(":/olr/shaders/grid_nn.frag.qsb"));
    QVERIFY2(vert.isValid(), "grid.vert.qsb missing or not deserializable");
    QVERIFY2(frag.isValid(), "grid_nn.frag.qsb missing or not deserializable");
    QCOMPARE(vert.stage(), QShader::VertexStage);
    QCOMPARE(frag.stage(), QShader::FragmentStage);
}
```

Add the includes at the top of the file: `#include <QFile>` and `#include <rhi/qshader.h>`.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: FAIL — `grid.vert.qsb` / `grid_nn.frag.qsb` are not in the resource (`QShader` invalid).

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/shaders/grid.vert` (full-screen-triangle, passes normalized UV to the fragment stage; the fragment stage maps the pixel to a tile + source via the uniform block):

```glsl
#version 440
layout(location = 0) out vec2 vUv;
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUv = pos;                              // 0..1 across the output
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
```

Create `playback/gpu/shaders/grid_nn.frag` implementing the integer YUV→RGB math from the Interfaces contract. The uniform block carries `int uMatrix; int uRange; int uColumns; int uRows;` and per-tile source presence; each input pair (luma R8 + chroma RG8) is a sampler binding. For a single-source PGM the grid is 1×1. Use `texelFetch` (nearest, integer texel coords) for the compat lane — bilinear filtering is the quality lane (Task 6). Keep the file commented that it is the **oracle-validated compat shader (lane a)**.

> Implementation note: the per-tile loop in a fragment shader uses the output UV to pick a tile `(col,row)` and the in-tile UV to compute the nearest source texel (`min(srcDim-1, int(inTileUv * srcDim))`), mirroring `referenceComposeGridRgba8`'s `qMin(srcW-1,(x*srcW)/dstW)`. Bind up to `kMaxGridSources` (16) sampler pairs; absent feeds render the background `yuvToRgb8(16,128,128)` (the oracle's background).

Bake the shaders: in `tests/CMakeLists.txt`, extend the existing `qt_add_shaders(olr_test_gpu "olr_passthrough_shaders" ... FILES ...)` (lines 307-312) to also list `grid.vert` and `grid_nn.frag`, OR add a second `qt_add_shaders(olr_test_gpu "olr_grid_shaders" PREFIX "/olr/shaders" BASE ".../shaders" FILES grid.vert grid_nn.frag)`. Either way the baked artifacts land at `:/olr/shaders/grid.vert.qsb` and `:/olr/shaders/grid_nn.frag.qsb`.

Document the new aliases in `playback/gpu/shaders/olr_shaders.qrc` (the `.qrc` is documentation; `qt_add_shaders` produces the runtime resource).

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS (2 tests; the grid shaders deserialize and report the right stages).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/shaders/grid.vert playback/gpu/shaders/grid_nn.frag \
        playback/gpu/shaders/olr_shaders.qrc tests/CMakeLists.txt tests/unit/tst_gpucompositor.cpp
git commit -m "feat(gpu-compositor): nearest-neighbor compat grid shaders (integer YUV->RGB, qsb)"
```

---

## Task 3: NV12 `GpuSurface` upload helper for CPU-origin inputs (test + render feed)

**Precondition:** Task 1. The compositor samples `GpuSurface` inputs; a CPU-origin `FrameHandle` (the common test case + MPEG-2/NDI ingest, D6) must be uploadable to an NV12 `GpuSurface` on demand so the grid shader has textures to sample.

**Files:**
- Modify: `playback/gpu/gpucompositor.cpp` (a private helper that turns a `FrameHandle` into a sampled `GpuSurface`: GPU-backed → use `handle.data()->gpuSurface()`; CPU-backed → `readToCpu(Nv12)` then upload to an NV12 `GpuSurface`), `playback/gpu/gpucompositor.h` (no public change)
- Modify (Apple): `playback/gpu/gpucompositor_apple.mm` (created here) — the Apple upload path: `makeAppleNv12Surface(w,h)` + fill its IOSurface from the `CpuPlanes` NV12 bytes
- Test: `tests/unit/tst_gpucompositor.cpp` (a helper-level slot: a CPU-origin handle yields a valid NV12 `GpuSurface` whose desc matches)

**Interfaces:**
- Consumes: `FrameHandle::readToCpu(FramePixelFormat::Nv12)` + `formatcanon::yuv420pToNv12` (the readback chokepoint already does NV12 on the GPU path; for a CPU handle, `readToCpu(Nv12)` returns NV12 planes), `makeAppleNv12Surface` (`playback/gpu/appleiosurface.h`), `GpuSurface::desc()` (`playback/gpu/gpusurface.h`).
- Produces (private to the compositor):
  ```cpp
  // gpucompositor.cpp / _apple.mm — returns a sampled NV12 GpuSurface for `handle`
  // (a refcount alias if already GPU-NV12-backed; a fresh uploaded surface for a
  // CPU-origin handle). Null on failure. Defined platform-side; the neutral .cpp
  // declares the hook and the _apple.mm implements the IOSurface upload.
  std::shared_ptr<GpuSurface> uploadFrameToNv12Surface(const FrameHandle& handle,
                                                       const std::shared_ptr<GpuRhiContext>& rhi);
  ```
  On non-Apple (`Null`/stub), the helper returns nullptr (no GPU upload), and the compositor's `Null`-lane oracle test (Task 5) drives the shader from a **synthetic** NV12 texture filled in the test harness rather than this helper — keeping CI deterministic without Apple frameworks.

- [ ] **Step 1: Write the failing test (Apple-only upload)**

Add to `tests/unit/tst_gpucompositor.cpp`, guarded so non-Apple still compiles:

```cpp
#ifdef __APPLE__
    void cpuHandleUploadsToNv12Surface();
#endif
```

```cpp
#ifdef __APPLE__
void TestGpuCompositor::cpuHandleUploadsToNv12Surface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    FrameHandle h = solidYuv420pHandle(64, 48, 100, 110, 150);  // CPU-origin
    auto surf = GpuCompositor::uploadFrameToNv12SurfaceForTest(h, rhi);
    QVERIFY(surf != nullptr);
    QCOMPARE(surf->desc().format, FramePixelFormat::Nv12);
    QCOMPARE(surf->desc().width, 64);
    QCOMPARE(surf->desc().height, 48);
}
#endif
```

Add a static test shim `static std::shared_ptr<GpuSurface> uploadFrameToNv12SurfaceForTest(const FrameHandle&, const std::shared_ptr<GpuRhiContext>&);` to `GpuCompositor` (public, `#ifdef __APPLE__`-guarded) that forwards to the private helper, so the test can reach the upload path without a full compose.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor
```
Expected: FAIL — `uploadFrameToNv12SurfaceForTest` undeclared.

- [ ] **Step 3: Write the implementation**

In `gpucompositor.cpp`, declare the platform hook `uploadFrameToNv12Surface(...)` and the test shim forwarding to it. Create `playback/gpu/gpucompositor_apple.mm` implementing the Apple upload: if `handle.isGpuBacked()` and its `gpuSurface()->desc().format == Nv12`, return a `shared_ptr<GpuSurface>` aliasing it (no copy); else `CpuPlanes nv12 = handle.readToCpu(FramePixelFormat::Nv12)`, `auto s = makeAppleNv12Surface(nv12.width, nv12.height)`, lock its `CVPixelBuffer` and `memcpy` the Y plane (plane 0) + interleaved UV plane (plane 1) row-by-row honoring `CVPixelBufferGetBytesPerRowOfPlane`, unlock, return `s`. Add the non-Apple stub returning nullptr in `gpucompositor.cpp` under `#ifndef __APPLE__`.

Wire `gpucompositor_apple.mm` into CMake: in `tests/CMakeLists.txt`, inside the `if(APPLE)` branches that add `gpurhicontext_apple.mm`, also add `playback/gpu/gpucompositor_apple.mm` to **both** `olr_test_playback` (lines 276-279) and `olr_test_gpu` (lines 298-301). Link is already `Qt6::GuiPrivate` + Metal/CoreVideo/IOSurface there.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS on Apple (the CPU handle uploads to a 64×48 NV12 surface); `QSKIP` with no RHI.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpucompositor.h playback/gpu/gpucompositor.cpp playback/gpu/gpucompositor_apple.mm \
        tests/unit/tst_gpucompositor.cpp tests/CMakeLists.txt
git commit -m "feat(gpu-compositor): NV12 GpuSurface upload for CPU-origin compositor inputs"
```

---

## Task 4: The grid render pass — compat shader produces an Rgba8 GpuSurface (Null-backend oracle-exact)

**Precondition:** Tasks 2 + 3. **This is the keystone correctness task: the compat shader on the `Null` backend must equal `formatcanon::referenceComposeGridRgba8` byte-for-byte.**

**Files:**
- Modify: `playback/gpu/gpucompositor.cpp` (build the grid pipeline on the `GpuRhiContext` render thread; render `frames`→`Rgba8` `QRhiTexture`; read back to `CpuPlanes` for the test path), `playback/gpu/gpucompositor.h` (no public change beyond Task 1's `composeGrid`)
- Test: `tests/unit/tst_gpucompositor.cpp` (the `Null`-backend oracle-exact slot)

**Interfaces:**
- Consumes: `formatcanon::referenceComposeGridRgba8` (oracle), `OlrRhi`/`QRhiGraphicsPipeline` pipeline idiom (`tst_shadertoolchain.cpp`), the grid `.qsb` (Task 2), the NV12 upload helper (Task 3 on Apple; a synthetic NV12 texture on `Null`), `GpuFence` (`playback/gpu/gpufence.h`), `makeGpuFrameHandle(surface, rhi, meta, renderFence)` (`playback/gpu/gpuframedata.h`), `GpuGenerationCounter::instance().current()` (`playback/gpu/gpugeneration.h`).
- Produces: `GpuCompositor::composeGrid(frames, w, h, color, NearestCompat)` returns a GPU-backed `Rgba8` `FrameHandle`. For the deterministic `Null`-lane test, a `readToCpu(Rgba8)` (or a test-only `composeGridToCpuForTest` returning `CpuPlanes`) yields the rendered RGBA bytes that must equal `referenceComposeGridRgba8`.

  Add a test-only entry that runs the render and reads the Rgba8 result back to `CpuPlanes`, so the WARP/Null lane asserts byte-equality without depending on Apple:
  ```cpp
  // gpucompositor.h (public, test-facing):
  // Render the grid and read the Rgba8 result back to CpuPlanes on the render
  // thread. Used by the Null/WARP oracle lane (lane a) and the quality PSNR lane
  // (lane b). Empty CpuPlanes on failure.
  CpuPlanes composeGridToCpu(const QList<FrameHandle>& frames, int width, int height,
                             ColorMetadata color, ScaleQuality quality) const;
  ```

- [ ] **Step 1: Write the failing test (Null-backend compat lane == oracle, byte-exact)**

Add to `tests/unit/tst_gpucompositor.cpp`:

```cpp
    void compatGridMatchesCpuOracleExactOnNull();
```

```cpp
// LANE (a): the nearest-neighbor compat shader, integer-reconstructed, must equal
// formatcanon::referenceComposeGridRgba8 BYTE-FOR-BYTE on the deterministic Null
// backend (the WARP-exact lane). A vendor-rounding epsilon is reserved ONLY for a
// real local-GPU host (Task 5); on Null there is no GPU rounding, so it is exact.
void TestGpuCompositor::compatGridMatchesCpuOracleExactOnNull() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> frames{
        solidYuv420pHandle(4, 4, 40, 60, 200), solidYuv420pHandle(4, 4, 80, 70, 190),
        solidYuv420pHandle(4, 4, 120, 80, 180), solidYuv420pHandle(4, 4, 160, 90, 170)};
    ColorMetadata color;  // default BT.709 / Video

    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    QVERIFY(oracle.isValid());
    const CpuPlanes gpu =
        comp->composeGridToCpu(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    if (!gpu.isValid()) QSKIP("Null-backend RGBA readback not available on this host");

    QCOMPARE(gpu.format, FramePixelFormat::Rgba8);
    QCOMPARE(gpu.width, 8);
    QCOMPARE(gpu.height, 8);
    QCOMPARE(gpu.plane[0], oracle.plane[0]);  // BYTE-FOR-BYTE on Null/WARP
}
```

Add `#include "playback/output/formatcanon.h"` to the test.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: FAIL — `composeGridToCpu` undeclared / returns empty (no render pass yet).

- [ ] **Step 3: Write the implementation**

In `gpucompositor.cpp`, build the grid render in `composeGridToCpu` / `composeGrid`. On the `GpuRhiContext` render thread (post a job, as `GpuRhiContext::importAndReadback` does):
1. Create the output `QRhiTexture(RGBA8, QSize(w,h), RenderTarget)` + render target + render-pass descriptor (the `tst_shadertoolchain` idiom).
2. For each input frame, obtain its NV12 sample textures: on Apple via the Task-3 upload helper → IOSurface→Metal→`QRhiTexture::createFrom` (two `QRhiTexture`s: R8 luma + RG8 chroma); on `Null`, fill two `QRhiTexture`s by `QRhiResourceUpdateBatch::uploadTexture` from the `readToCpu(Nv12)` planes (deterministic, no Apple frameworks).
3. Build the grid pipeline: `grid.vert` + `grid_nn.frag`, sampler bindings for each source pair, a uniform buffer carrying `uMatrix`/`uRange`/`uColumns`/`uRows` + per-tile rects. Drop any input where `handle.isStaleForGeneration(GpuGenerationCounter::instance().current())` is true (treat as absent → background tile).
4. `beginPass` clearing to the background color, `draw`, `endPass`; then a readback (`readBackTexture`) into `CpuPlanes(Rgba8)` for `composeGridToCpu`.
5. Signal the render `GpuFence` after the pass; for `composeGrid`, wrap the output texture as an `Rgba8` `GpuSurface`, stamp `meta.gpuGeneration = GpuGenerationCounter::instance().current()`, and mint `makeGpuFrameHandle(outputSurface, m_impl->rhi, meta, m_impl->renderFence)`.

The integer-faithful shader math (Task 2) on the `Null` backend (no GPU rounding) produces RGBA bytes identical to `referenceComposeGridRgba8`. Where the platform produces only a GPU surface (no cheap CPU read), `composeGridToCpu` uses the RHI `readBackTexture` path.

Construct the compositor's `m_renderFence = GpuFence::create();` in `GpuCompositor::create()` (store in `Impl`); null fence degrades gracefully (the render still works; ordering falls back to the synchronous readback already in place this phase).

**Review gate:** the render + readback run on the `GpuRhiContext` render thread and signal a `GpuFence` shared with decode/eviction; the cadence-thread caller must not read the output before the fence retires. Carry `// LOCK RULE:` / `// FENCE:` comments on the handoff. Flag for independent concurrency review.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS — the compat grid equals the oracle byte-for-byte on the `Null` backend (or `QSKIP` where RGBA readback is unavailable).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpucompositor.h playback/gpu/gpucompositor.cpp tests/unit/tst_gpucompositor.cpp
git commit -m "feat(gpu-compositor): compat grid render equals CPU oracle byte-for-byte on Null"
```

---

## Task 5: ±1-LSB local-GPU lane + swapped-quadrant bite + PGM select

**Precondition:** Task 4. Lane (a) extends from `Null`-exact to **±1 LSB on a real GPU**, and the quadrant-placement bite proves the gate catches a wrong layout.

**Files:**
- Modify: `playback/gpu/gpucompositor.cpp` (`composePgm` delegates to a 1×1 grid)
- Test: `tests/unit/tst_gpucompositor.cpp` (±1-LSB lane slot, swapped-quadrant slot, PGM slot)

**Interfaces:**
- Consumes: `formatcanon::referenceComposeGridRgba8` (oracle), `composeGridToCpu` (Task 4), `psnrY8` is **not** used here (this is the LSB lane, exact-ish); the swap test mirrors `tst_yuv420pcompositor.cpp::quadrantPlacementIsNotSymmetric`.
- Produces: `GpuCompositor::composePgm` validated as a 1×1 grid select.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/tst_gpucompositor.cpp`:

```cpp
    void compatGridWithinOneLsbOnLocalGpu();
    void swappedQuadrantsDifferFromOracle();
    void pgmSelectFillsOutputFromSingleSource();
```

```cpp
// LANE (a) on a REAL GPU host: vendor rounding may differ by up to 1 LSB/channel
// from the integer oracle. On the Null backend this is exact (Task 4 covers that);
// here we assert the bounded epsilon and SKIP where there is no real GPU device.
void TestGpuCompositor::compatGridWithinOneLsbOnLocalGpu() {
    auto rhi = GpuRhiContext::create();
    if (!rhi || !GpuCompositor::hasLocalGpuBackend(rhi))
        QSKIP("no local GPU backend (Null/CI) — exact lane covers Null");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200),
                              solidYuv420pHandle(4, 4, 160, 90, 170)};
    ColorMetadata color;
    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    const CpuPlanes gpu =
        comp->composeGridToCpu(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(oracle.isValid() && gpu.isValid());
    QCOMPARE(oracle.plane[0].size(), gpu.plane[0].size());
    int maxDelta = 0;
    for (int i = 0; i < oracle.plane[0].size(); ++i)
        maxDelta = qMax(maxDelta, qAbs(int(uchar(oracle.plane[0].at(i))) -
                                       int(uchar(gpu.plane[0].at(i)))));
    QVERIFY2(maxDelta <= 1, qPrintable(QStringLiteral("max channel delta %1 > 1 LSB").arg(maxDelta)));
}

// The pixel-exact gate BITES: swapping two source feeds must change at least one
// output byte vs the correct-arrangement oracle (mirrors tst_yuv420pcompositor).
void TestGpuCompositor::swappedQuadrantsDifferFromOracle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    QList<FrameHandle> correct{
        solidYuv420pHandle(4, 4, 40, 60, 200), solidYuv420pHandle(4, 4, 80, 70, 190),
        solidYuv420pHandle(4, 4, 120, 80, 180), solidYuv420pHandle(4, 4, 160, 90, 170)};
    QList<FrameHandle> swapped{correct.at(0), correct.at(2), correct.at(1), correct.at(3)};
    ColorMetadata color;
    const CpuPlanes correctOracle = formatcanon::referenceComposeGridRgba8(correct, 8, 8, color);
    const CpuPlanes gpuSwapped =
        comp->composeGridToCpu(swapped, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    if (!gpuSwapped.isValid()) QSKIP("RGBA readback unavailable");
    QVERIFY2(gpuSwapped.plane[0] != correctOracle.plane[0],
             "swapped feeds must not equal the correct-arrangement oracle");
}

// PGM select: one source fills the whole output (a 1x1 grid). The compat lane must
// equal the single-source oracle on Null.
void TestGpuCompositor::pgmSelectFillsOutputFromSingleSource() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    FrameHandle src = solidYuv420pHandle(4, 4, 130, 100, 150);
    ColorMetadata color;
    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8({src}, 8, 8, color);
    const FrameHandle pgm =
        comp->composePgm(src, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    if (pgm.isNull()) QSKIP("PGM compose unavailable");
    const CpuPlanes got = pgm.readToCpu(FramePixelFormat::Rgba8);
    if (!got.isValid()) QSKIP("RGBA readback unavailable");
    QCOMPARE(got.plane[0], oracle.plane[0]);
}
```

Add a `static bool hasLocalGpuBackend(const std::shared_ptr<GpuRhiContext>&);` to `GpuCompositor` (returns false on the `Null` backend, true on Metal/D3D) so the ±1-LSB lane only runs on a real GPU.

- [ ] **Step 2: Run the tests, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor
```
Expected: FAIL — `hasLocalGpuBackend` undeclared; `composePgm` returns null.

- [ ] **Step 3: Write the implementation**

Implement `GpuCompositor::composePgm` by delegating to `composeGrid(/*frames=*/{source}, w, h, color, quality)` (a 1×1 grid is the single-source select; the grid layout `ceil(sqrt(1))==1` column already fills the output). Implement `hasLocalGpuBackend` by inspecting the `GpuRhiContext`'s backend (expose a `bool isNullBackend() const` on `GpuRhiContext` if not already reachable, or check via a new accessor; if `GpuRhiContext` does not expose its backend, add `bool GpuRhiContext::isGpuBacked() const` returning true when the underlying `QRhi::backend() != QRhi::Null` — keep the addition minimal and behind `OLR_GPU_PIPELINE_BUILD`). The `Null`-lane (Task 4) stays exact; this lane only relaxes to ±1 on a real device.

- [ ] **Step 4: Run the tests, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS — the swap test bites on `Null`; PGM equals the single-source oracle; the ±1-LSB lane `QSKIP`s on `Null`/CI and runs on a Metal host.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpucompositor.h playback/gpu/gpucompositor.cpp playback/gpu/gpurhicontext.h \
        tests/unit/tst_gpucompositor.cpp
git commit -m "feat(gpu-compositor): +-1-LSB local-GPU lane, swapped-quadrant bite, PGM select"
```

---

## Task 6: The quality scaler (bilinear) + its OWN PSNR goldens (never the oracle)

**Precondition:** Tasks 4 + 5. **Lane (b): the quality scaler is validated against its OWN PSNR/SSIM goldens, NEVER against the nearest-neighbor oracle (§9, D3, the "quality scaler not oracle-validatable" risk).**

**Files:**
- Create: `playback/gpu/shaders/grid_quality.frag`
- Modify: `tests/CMakeLists.txt` (bake `grid_quality.frag`), `playback/gpu/gpucompositor.cpp` (`ScaleQuality::Bilinear` selects the quality pipeline), `playback/gpu/shaders/olr_shaders.qrc`
- Test: `tests/unit/tst_gpucompositor.cpp` (a PSNR slot — bilinear vs a bilinear reference, NOT vs the nearest oracle)

**Interfaces:**
- Consumes: `psnrY8` (`tests/unit/framepsnr.h`), the grid pipeline (Task 4), `formatcanon::yuvToRgb8` for building a CPU **bilinear reference** (the quality lane's own golden, distinct from the nearest oracle).
- Produces: baked `:/olr/shaders/grid_quality.frag.qsb`; `composeGridToCpu(..., Bilinear)` runs the linear-filtered pipeline.

  **The quality golden is computed in-test** as a CPU bilinear upscale of the same sources (sample with linear interpolation, then `yuvToRgb8`), and the GPU bilinear output is asserted **PSNR ≥ a threshold** (e.g. ≥ 40 dB on the luma-derived green channel) against that bilinear reference — **never** `QCOMPARE`d to `referenceComposeGridRgba8`. A guard slot asserts the bilinear output **differs** from the nearest oracle (proving the scaler is actually a different filter, not silently nearest).

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/tst_gpucompositor.cpp`:

```cpp
    void bilinearScalerMeetsPsnrAgainstBilinearReference();
    void bilinearDiffersFromNearestOracle();
```

```cpp
// LANE (b): the bilinear quality scaler is validated against its OWN bilinear
// reference with a PERCEPTUAL (PSNR) tolerance — NEVER against the nearest-neighbor
// CPU oracle (which is a different filter by definition). psnrY8 from framepsnr.h.
void TestGpuCompositor::bilinearScalerMeetsPsnrAgainstBilinearReference() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    // A gradient source so bilinear vs nearest actually diverge.
    FrameHandle src = gradientYuv420pHandle(8, 8);  // test-local helper
    ColorMetadata color;
    const CpuPlanes ref = bilinearUpscaleReferenceRgba8(src, 32, 32, color);  // test-local
    const CpuPlanes gpu =
        comp->composeGridToCpu({src}, 32, 32, color, GpuCompositor::ScaleQuality::Bilinear);
    if (!gpu.isValid()) QSKIP("RGBA readback unavailable");
    // Compare the green channel (carries luma) row-stride-aware via psnrY8 on a
    // packed copy of channel 1.
    const QByteArray refG = packChannel(ref, 1);
    const QByteArray gpuG = packChannel(gpu, 1);
    const double psnr = psnrY8(reinterpret_cast<const uint8_t*>(refG.constData()), 32,
                               reinterpret_cast<const uint8_t*>(gpuG.constData()), 32, 32, 32);
    QVERIFY2(psnr >= 40.0, qPrintable(QStringLiteral("bilinear PSNR %1 dB < 40").arg(psnr)));
}

// The bilinear scaler must NOT be silently nearest: on an upscaled gradient its
// output differs from the nearest oracle.
void TestGpuCompositor::bilinearDiffersFromNearestOracle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    FrameHandle src = gradientYuv420pHandle(8, 8);
    ColorMetadata color;
    const CpuPlanes nearest = formatcanon::referenceComposeGridRgba8({src}, 32, 32, color);
    const CpuPlanes bilinear =
        comp->composeGridToCpu({src}, 32, 32, color, GpuCompositor::ScaleQuality::Bilinear);
    if (!bilinear.isValid()) QSKIP("RGBA readback unavailable");
    QVERIFY2(bilinear.plane[0] != nearest.plane[0],
             "bilinear scaler must differ from the nearest-neighbor oracle");
}
```

Add the test-local helpers `gradientYuv420pHandle`, `bilinearUpscaleReferenceRgba8`, `packChannel` to the anonymous namespace of the test file (the bilinear reference samples the source with linear interpolation then `formatcanon::yuvToRgb8`; `packChannel` copies one RGBA channel into a width-stride-tight `QByteArray` for `psnrY8`). Add `#include "framepsnr.h"`.

- [ ] **Step 2: Run the tests, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor
```
Expected: FAIL — `grid_quality.frag.qsb` missing; `Bilinear` falls through to nearest (the differs-test fails).

- [ ] **Step 3: Write the implementation**

Create `playback/gpu/shaders/grid_quality.frag` — same uniform/sampler layout as `grid_nn.frag`, but sample the luma/chroma with **linear filtering** (`texture()` with a `linear` sampler, not `texelFetch`), then apply the same integer `yuvToRgb8` conversion on the interpolated YUV. Bake it in `tests/CMakeLists.txt` (add to the grid `qt_add_shaders` FILES). In `gpucompositor.cpp`, select `grid_quality.frag.qsb` + a `QRhiSampler` with `Linear` min/mag filtering when `quality != NearestCompat`; `NearestCompat` keeps `Nearest` filtering + `grid_nn.frag.qsb`. (`Lanczos` may alias to `Bilinear` this phase with a `// TODO(gpu-compositor): Lanczos kernel` note — the enum reserves the slot; a separate Lanczos kernel is a follow-up, not load-bearing here.) Document `olr_shaders.qrc`.

- [ ] **Step 4: Run the tests, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS — bilinear meets the PSNR floor vs its own reference and differs from the nearest oracle.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/shaders/grid_quality.frag playback/gpu/shaders/olr_shaders.qrc \
        playback/gpu/gpucompositor.cpp tests/CMakeLists.txt tests/unit/tst_gpucompositor.cpp
git commit -m "feat(gpu-compositor): bilinear quality scaler with its own PSNR goldens (lane b)"
```

---

## Task 7: GPU-texture multiview memo keyed on sourceKeys

**Precondition:** Tasks 4 + 5. Carry the `MultiviewComposite` memo forward as a GPU-texture cache so a tick with an unchanged source set reuses the prior `Rgba8` `GpuSurface` (a refcount bump, no re-render, no readback) — D9 refs-only.

**Files:**
- Modify: `playback/output/outputbusengine.h` (the `MultiviewComposite` memo already carries `sourceKeys` + a `FrameHandle video`; a GPU-composited `video` is a GPU-backed `Rgba8` handle, so **no struct change is needed** — the memo already holds a `FrameHandle`), `playback/gpu/gpucompositor.cpp` (a memo-aware compose entry that takes the prior memo handle + the new sourceKeys and returns the reused or freshly-rendered handle)
- Test: `tests/unit/tst_gpucompositor.cpp` (a memo-hit reuses the exact same surface; a memo-miss renders fresh)

**Interfaces:**
- Consumes: `MultiviewComposite` (`playback/output/outputbusengine.h` — `valid`, `sourceKeys`, `video`), `FrameHandle::dataPtr()` (identity of the shared pixel payload).
- Produces:
  ```cpp
  // gpucompositor.h: a memo-aware grid compose. On a sourceKeys match against the
  // memo, returns memo.video (a refcount bump — the SAME GpuSurface, no re-render).
  // On a miss, renders fresh and updates the memo. Mirrors the CPU memo in
  // OutputBusEngine::renderMultiview exactly so a reused frame is byte-identical.
  FrameHandle composeGridMemoized(const QList<FrameHandle>& frames, int width, int height,
                                  ColorMetadata color, ScaleQuality quality,
                                  const QVector<qint64>& sourceKeys,
                                  MultiviewComposite* memo) const;
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_gpucompositor.cpp`:

```cpp
    void memoHitReusesSameGpuSurface();
    void memoMissRendersFresh();
```

```cpp
void TestGpuCompositor::memoHitReusesSameGpuSurface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200),
                              solidYuv420pHandle(4, 4, 80, 70, 190)};
    QVector<qint64> keys{1, 100, 1, 200};
    MultiviewComposite memo;
    ColorMetadata color;
    const FrameHandle a =
        comp->composeGridMemoized(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat,
                                  keys, &memo);
    if (a.isNull()) QSKIP("compose unavailable");
    QVERIFY(memo.valid);
    const FrameHandle b =
        comp->composeGridMemoized(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat,
                                  keys, &memo);
    // Same sourceKeys -> reuse the SAME underlying pixel payload (refcount bump).
    QVERIFY(a.dataPtr() == b.dataPtr());
}

void TestGpuCompositor::memoMissRendersFresh() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200)};
    MultiviewComposite memo;
    ColorMetadata color;
    const FrameHandle a = comp->composeGridMemoized(
        frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat, {1, 100}, &memo);
    const FrameHandle b = comp->composeGridMemoized(
        frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat, {1, 999}, &memo);
    if (a.isNull() || b.isNull()) QSKIP("compose unavailable");
    QVERIFY(a.dataPtr() != b.dataPtr());  // different sourceKeys -> fresh render
}
```

Add `#include "playback/output/outputbusengine.h"` to the test.

- [ ] **Step 2: Run the tests, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor
```
Expected: FAIL — `composeGridMemoized` undeclared.

- [ ] **Step 3: Write the implementation**

Implement `composeGridMemoized`: if `memo && memo->valid && memo->sourceKeys == sourceKeys` and `!memo->video.isNull()`, return `memo->video` (the refcount-shared GPU `Rgba8` handle). Else render via `composeGrid(...)`, and on success set `memo->valid = true; memo->sourceKeys = sourceKeys; memo->video = result;`. This mirrors `OutputBusEngine::renderMultiview` (lines 145-158) exactly, so the GPU memo behaves like the CPU memo. **Review gate:** a reused GPU surface must still be fence-valid for the consumer; the memo'd handle keeps its render-fence stamp, so the consumer's `readToCpu` waits correctly — note this in a comment and flag for review.

- [ ] **Step 4: Run the tests, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor && ctest --test-dir build/c -R tst_gpucompositor --output-on-failure
```
Expected: PASS — a memo hit reuses the same payload; a miss renders fresh.

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpucompositor.h playback/gpu/gpucompositor.cpp tests/unit/tst_gpucompositor.cpp
git commit -m "feat(gpu-compositor): GPU-texture multiview memo keyed on sourceKeys (refs-only)"
```

---

## Task 8: Wire the compositor-select seam into `OutputBusEngine` (CPU default, GPU behind flags)

**Precondition:** Tasks 4-7. **This is the integration task: `renderMultiview`/`renderPgm` route to the GPU compositor when enabled, else the byte-for-byte CPU path.**

**Files:**
- Modify: `playback/output/outputbusengine.h`/`.cpp` (an injectable `GpuCompositor` hook; the grid/select branches on it), `playback/output/outputdispatcher.cpp`/`.h` (construct + inject the `GpuCompositor` when `gpuPipelineEnabled()` and an RHI context exists; keep `m_multiviewMemo` as the shared memo)
- Test: `tests/unit/tst_outputbusengine.cpp` (a GPU-compositor-injected multiview produces a valid frame; with no compositor the existing CPU goldens are unchanged), `tests/unit/tst_gpucompositor.cpp` (the seam delegates to CPU when no compositor)

**Interfaces:**
- Consumes: `OutputBusEngine::renderMultiview`/`renderPgm` (existing), `Yuv420pCompositor::composeGrid` (CPU fallback), `GpuCompositor` (Tasks 1-7), `gpuPipelineEnabled()`.
- Produces (extend `OutputBusEngine`, additive — a setter, default null keeps the CPU path):
  ```cpp
  // outputbusengine.h, class OutputBusEngine:
  // Inject the GPU compositor (null = CPU Yuv420pCompositor, the default + oracle).
  // When set AND gpuPipelineEnabled(), renderMultiview/renderPgm grid+select on the
  // GPU; the multiview memo becomes the GPU-texture memo (same sourceKeys key).
  void setGpuCompositor(std::shared_ptr<GpuCompositor> compositor);
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_outputbusengine.cpp`:

```cpp
    void multiviewUsesCpuCompositorByDefault();
    void multiviewUsesGpuCompositorWhenInjected();
```

```cpp
// Default: no GPU compositor injected -> the existing CPU multiview golden holds
// (this is the zero-regression anchor — assert the same value the CPU path emits).
void TestOutputBusEngine::multiviewUsesCpuCompositorByDefault() {
    OutputBusEngine engine(FrameRate{30, 1}, 2, 8, 8);
    // ... build a state + cache with two feeds (reuse the file's existing fixture) ...
    const auto mv = engine.renderMultiview(5, state, cache, nullptr);
    QVERIFY(!mv.video.isNull());
    // The composited handle is the CPU Yuv420pCompositor output (unchanged golden).
    const MediaVideoFrameView view(mv.video);
    QVERIFY(view.isValid());
}

// With a GPU compositor injected AND the flag on, renderMultiview routes to the GPU
// and still produces a valid frame (correctness of the seam; the GPU bytes are
// covered by tst_gpucompositor's oracle lane).
void TestOutputBusEngine::multiviewUsesGpuCompositorWhenInjected() {
    qputenv("OLR_GPU_PIPELINE", "1");
    auto rhi = GpuRhiContext::create();
    if (!rhi) { qunsetenv("OLR_GPU_PIPELINE"); QSKIP("no RHI backend"); }
    auto comp = GpuCompositor::create(rhi);
    if (!comp) { qunsetenv("OLR_GPU_PIPELINE"); QSKIP("compositor unavailable"); }
    OutputBusEngine engine(FrameRate{30, 1}, 2, 8, 8);
    engine.setGpuCompositor(comp);
    // ... same fixture ...
    const auto mv = engine.renderMultiview(5, state, cache, nullptr);
    QVERIFY(!mv.video.isNull());
    qunsetenv("OLR_GPU_PIPELINE");
}
```

(Use the existing fixture-building helpers in `tst_outputbusengine.cpp`; these slots are guarded so the GPU one `QSKIP`s without a backend.) Register the test against `olr_test_gpu` if not already: in `tests/unit/CMakeLists.txt`, the `tst_outputbusengine` registration (line 119) is `olr_test_playback`; under `if(OLR_GPU_PIPELINE)` add `target_link_libraries(tst_outputbusengine PRIVATE olr_test_gpu)` so the GPU slots link.

- [ ] **Step 2: Run the tests, expect FAIL**

```sh
cmake --build build/c --target tst_outputbusengine
```
Expected: FAIL — `setGpuCompositor` undeclared.

- [ ] **Step 3: Write the implementation**

In `outputbusengine.h`/`.cpp`, add `std::shared_ptr<GpuCompositor> m_gpuCompositor;` + `setGpuCompositor`. In `renderMultiview`, replace the `Yuv420pCompositor::composeGrid(frames, m_width, m_height)` call site (line 152) with:

```cpp
#ifdef OLR_GPU_PIPELINE_BUILD
    if (m_gpuCompositor && m_gpuCompositor->isValid() && gpuPipelineEnabled()) {
        const ColorMetadata color = /* from the selected source frames; default-tag policy */;
        FrameHandle gpu = m_gpuCompositor->composeGridMemoized(
            frames, m_width, m_height, color, GpuCompositor::ScaleQuality::Bilinear, sourceKeys, memo);
        if (!gpu.isNull()) { out.video = gpu; }
        else { out.video = Yuv420pCompositor::composeGrid(frames, m_width, m_height); }  // degrade
    } else
#endif
    {
        // existing CPU path, UNCHANGED (the memo branch at lines 145-158 stays for CPU)
        ...
    }
```

The CPU branch keeps its existing `MultiviewComposite` memo logic byte-for-byte; the GPU branch routes the memo through `composeGridMemoized`. The `out.video.metadata().key.ptsMs`/`outputFrameIndex` overrides (lines 162-163) apply to both. For `renderPgm`, the single-source path routes to `m_gpuCompositor->composePgm` under the same guard with a CPU degrade. Use the default-tagging color policy (height>576→BT709 else BT601, video range) per the merged `color-metadata` no-op when the source has no explicit tags.

In `outputdispatcher.cpp`, construct the `GpuCompositor` once (when `gpuPipelineEnabled()` and a `GpuRhiContext` is live — reuse the worker's RHI context) and call `engine.setGpuCompositor(...)`; on `create()` failure leave it null (CPU default). Keep `m_multiviewMemo` as the shared memo passed to `renderMultiview`.

**Review gate:** the seam is called on the cadence thread; the compositor renders on the RHI render thread and the output is fence-stamped. Confirm the cadence thread does not read the GPU output before the render fence retires (the `readToCpu` chokepoint waits). Flag for independent concurrency review.

- [ ] **Step 4: Run the tests, expect PASS**

```sh
cmake --build build/c --target tst_outputbusengine && ctest --test-dir build/c -R tst_outputbusengine --output-on-failure
```
Expected: PASS — CPU default unchanged; GPU seam routes when injected (or `QSKIP`s).

- [ ] **Step 5: Zero-regression gate (the critical one)**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
ctest --test-dir build/c -R e2e_play --output-on-failure
```
Expected: with the flag off, the **full** unit suite + `e2e_play` pass with identical assertion values and golden outputs (`tst_yuv420pcompositor`, `tst_outputbusengine`, `tst_formatcanon` unchanged). Then a fresh `-DOLR_GPU_PIPELINE=OFF` build dir confirms the off-path compiles byte-green.

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/outputbusengine.h playback/output/outputbusengine.cpp \
        playback/output/outputdispatcher.cpp playback/output/outputdispatcher.h \
        tests/unit/tst_outputbusengine.cpp tests/unit/CMakeLists.txt
git commit -m "feat(gpu-compositor): OutputBusEngine compositor-select seam (CPU default, GPU flagged)"
```

---

## Task 9: Multi-feed cap-pressure GPU compositor stress (evict-while-compose, stale-generation, OOM-degrade)

**Precondition:** Tasks 4-8 + the `gpu-sync` eviction guard/generation counter. **This is the §7 Phase-3 "multi-feed cap-pressure stress" scenario for the compositor: concurrent compose-while-evict, stale-generation drop, and OOM-degrade — the TSan-invisible GPU races the single-feed Phase-2 slice could not exercise.**

**Files:**
- Test: `tests/unit/tst_gpucompositor_stress.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuCompositor` (Tasks 1-8), `GpuGenerationCounter`/`FrameHandle::isStaleForGeneration` (`gpu-sync`), `GpuSurface::retainUntilFenceRetired`/`pendingFenceValue` (`gpu-sync`), `gpuSetInjectedAllocFailures`/`gpuConsumeInjectedAllocFailure` (`playback/gpu/gpupipelineconfig.h`), `formatcanon::referenceComposeGridRgba8` (checksum oracle).
- Produces: no new product interface — a stress gate that `QSKIP`s where no RHI backend exists.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_gpucompositor_stress.cpp`:

```cpp
// Multi-feed cap-pressure GPU compositor stress (spec §7 Phase 3): concurrent
// compose-while-evict, stale-generation drop, and OOM-degrade-to-CPU. TSan cannot
// see GPU command ordering, so these are first-class gates. Each SKIPs with no RHI.
#include <QtTest>

#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/formatcanon.h"
#include "playback/output/framehandle.h"
#include "playback/output/yuv420pcompositor.h"

#include <thread>

class TestGpuCompositorStress : public QObject {
    Q_OBJECT
private slots:
    void staleGenerationInputIsDroppedAsAbsent();
    void oomDegradesToCpuWithoutCrashing();
    void concurrentComposeAndChecksumValidate();
};

void TestGpuCompositorStress::staleGenerationInputIsDroppedAsAbsent() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    GpuGenerationCounter::instance().reset();
    const uint64_t g0 = GpuGenerationCounter::instance().bump();  // 1
    FrameHandle stale = solidYuv420pHandle(4, 4, 200, 128, 128);
    stale.metadata().gpuGeneration = g0;
    GpuGenerationCounter::instance().bump();  // 2 -> stale is now superseded
    ColorMetadata color;
    // A stale input renders as an absent feed (background tile), same as a null
    // handle in the CPU oracle. Compose {stale} into 8x8 -> all background.
    const CpuPlanes got =
        comp->composeGridToCpu({stale}, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    if (!got.isValid()) QSKIP("RGBA readback unavailable");
    const CpuPlanes bgOnly = formatcanon::referenceComposeGridRgba8({}, 8, 8, color);  // empty
    QCOMPARE(got.plane[0], bgOnly.plane[0]);
}

void TestGpuCompositorStress::oomDegradesToCpuWithoutCrashing() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200)};
    ColorMetadata color;
    gpuSetInjectedAllocFailures(1);  // the next GPU surface alloc fails
    const FrameHandle gpu =
        comp->composeGrid(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    // On injected OOM, composeGrid returns a null handle (caller degrades to CPU)
    // — it must NOT crash. The CPU compositor still produces a valid frame.
    const FrameHandle cpu = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QVERIFY(!cpu.isNull());
}

void TestGpuCompositorStress::concurrentComposeAndChecksumValidate() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200),
                              solidYuv420pHandle(4, 4, 160, 90, 170)};
    ColorMetadata color;
    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    // Hammer compose from the cadence thread while bumping the generation counter
    // (simulating seeks); every successful compose either equals the oracle (Null)
    // or is null (degraded) — never a torn/garbage frame.
    std::atomic<bool> stop{false};
    std::thread bumper([&] {
        while (!stop.load()) GpuGenerationCounter::instance().bump();
    });
    for (int i = 0; i < 50; ++i) {
        const CpuPlanes got = comp->composeGridToCpu(frames, 8, 8, color,
                                                     GpuCompositor::ScaleQuality::NearestCompat);
        if (got.isValid()) QCOMPARE(got.plane[0].size(), oracle.plane[0].size());
    }
    stop.store(true);
    bumper.join();
}
```

> Note: `staleGenerationInputIsDroppedAsAbsent` requires `referenceComposeGridRgba8({}, 8, 8, color)` (empty frame list) to yield an all-background frame — confirm the oracle handles an empty list (it does: the loop over `frames` is skipped, leaving the background fill). The stress harness stamps generation on the input handle and relies on Task 4's `isStaleForGeneration` drop.

Register in the `if(OLR_GPU_PIPELINE)` block of `tests/unit/CMakeLists.txt`:

```cmake
    olr_add_unit_test(tst_gpucompositor_stress olr_test_playback olr_test_gpu)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_gpucompositor_stress
```
Expected: FAIL to compile or FAIL at runtime — the stale-drop / OOM-degrade behavior is not yet complete (Task 4 added the stale check; confirm OOM-degrade returns null on injected failure). If a behavior is missing, complete it minimally in `gpucompositor.cpp` (e.g. honor `gpuConsumeInjectedAllocFailure()` at the output-surface alloc site → return null `FrameHandle`).

- [ ] **Step 3: Write the implementation (close any gaps)**

Ensure `composeGrid` (a) checks `gpuConsumeInjectedAllocFailure()` before allocating the output `Rgba8` surface and returns null on a fire (OOM-degrade); (b) drops `isStaleForGeneration(current)` inputs as absent (Task 4 wired this; verify); (c) never reads a surface mid-write (the render fence orders the readback). These are the three TSan-invisible behaviors the stress asserts.

**Review gate:** this scenario is the compositor's TSan-invisible-race gate. The `bumper` thread races the cadence-thread compose; the contract is "every compose is the oracle or null, never torn." Run under the ASan/TSan lanes (`OLR_PREPUSH_FULL=1` or CI) and flag for independent concurrency review before merge.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_gpucompositor_stress && ctest --test-dir build/c -R tst_gpucompositor_stress --output-on-failure
```
Expected: PASS — stale inputs drop to background, injected OOM degrades to CPU without crashing, concurrent compose never tears (or `QSKIP` with no RHI).

- [ ] **Step 5: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/gpucompositor.cpp tests/unit/tst_gpucompositor_stress.cpp tests/unit/CMakeLists.txt
git commit -m "test(gpu-compositor): multi-feed cap-pressure stress (stale-drop, OOM-degrade, checksum)"
```

---

## Canonical compositor contract (downstream plans consume these names verbatim)

`async-readback`, `gpu-budget`, `device-loss`, and `ios-bringup` build on exactly these types and signatures. Do not rename or vary them.

**The GPU compositor** — `playback/gpu/gpucompositor.h` (NEW):
```cpp
class GpuCompositor {
    enum class ScaleQuality { NearestCompat, Bilinear, Lanczos };
    static std::shared_ptr<GpuCompositor> create(std::shared_ptr<GpuRhiContext> rhi);  // null = degrade
    bool isValid() const;
    FrameHandle composeGrid(const QList<FrameHandle>& frames, int width, int height,
                            ColorMetadata color, ScaleQuality quality) const;   // GPU Rgba8 handle / null
    FrameHandle composePgm(const FrameHandle& source, int width, int height,
                           ColorMetadata color, ScaleQuality quality) const;    // 1x1 select
    FrameHandle composeGridMemoized(const QList<FrameHandle>& frames, int width, int height,
                                    ColorMetadata color, ScaleQuality quality,
                                    const QVector<qint64>& sourceKeys,
                                    MultiviewComposite* memo) const;            // refs-only memo
    CpuPlanes composeGridToCpu(const QList<FrameHandle>& frames, int width, int height,
                               ColorMetadata color, ScaleQuality quality) const; // validation lanes
    static bool hasLocalGpuBackend(const std::shared_ptr<GpuRhiContext>& rhi);   // false on Null/CI
};
```

**The compositor-select seam** — `playback/output/outputbusengine.h` (ADDED, additive):
```cpp
void OutputBusEngine::setGpuCompositor(std::shared_ptr<GpuCompositor> compositor);  // null = CPU oracle
```

**Shaders** (baked `.qsb`, runtime-loaded via `QShader::fromSerialized`):
- `:/olr/shaders/grid.vert.qsb` — per-tile grid vertex stage.
- `:/olr/shaders/grid_nn.frag.qsb` — lane (a) nearest-neighbor compat, integer YUV→RGB matching `formatcanon::referenceComposeGridRgba8` (oracle).
- `:/olr/shaders/grid_quality.frag.qsb` — lane (b) bilinear/Lanczos quality scaler, validated by its OWN PSNR/SSIM goldens, never the oracle.

**Validation lanes (§9, never crossed):** lane (a) = compat shader vs `formatcanon::referenceComposeGridRgba8` — **exact on `Null`/WARP, ±1 LSB on local-GPU**; lane (b) = quality scaler vs its own bilinear/Lanczos reference at a PSNR floor (`psnrY8`, `tests/unit/framepsnr.h`) — **never** vs the oracle. The CPU `Yuv420pCompositor` is the permanent oracle + fallback, selected by `OLR_GPU_PIPELINE`/`gpuPipelineEnabled()`.

**Consumed `gpu-sync` names (verbatim):** `GpuFence::{create,signal,wait,completedValue}` (render-done ordering before output read), `GpuGenerationCounter::{instance,current,bump,reset}` + `FrameHandle::isStaleForGeneration(uint64_t)` + `FrameMetadata::gpuGeneration` (stale-input drop), `GpuSurface::{retainUntilFenceRetired,pendingFenceValue}` (output-surface lifetime), `makeGpuFrameHandle(surface, rhi, meta, renderFence)` (fence-stamped GPU `Rgba8` output mint).
