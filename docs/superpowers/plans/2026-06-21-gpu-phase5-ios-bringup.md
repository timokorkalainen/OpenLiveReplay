# ios-bringup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Bring the already-merged GPU-resident Metal pipeline up on iOS — **not** a new backend. The
edge (`GpuSurface`, `GpuRhiContext`, `GpuFrameData`, `DecodeDoneFence`, `VtKeepSurfaceImporter`) was
kept platform-symmetric and is already `#ifdef __APPLE__`, so on iOS arm64 it compiles from the same
`.mm` files. The deliverable is therefore the three **iOS-specific constraints** that macOS does not
pay: (1) **main-thread render affinity** — define how the dedicated `GpuRenderThread` (D11) interacts
with the iOS UIKit/`CAMetalLayer` main-thread presentation rule, so the GPU pipeline never touches
UIKit off the main thread; (2) **thermal throttling + VRAM limits** — bound the GPU window depth on
iOS more aggressively than macOS, expressed through the `gpu-budget` cap so iOS picks a documented,
smaller budget; (3) **background/foreground GPU resource handling** — release and reacquire GPU
resources on `UIApplication` suspend/resume, reusing the `device-loss`-style invalidation seam
(`GpuGenerationCounter` bump + surface invalidation + degrade-to-CPU). The existing `ios/ios_scene`
path is reused; no new app entry point. Smoke coverage: (a) the iOS Xcode build target compiles with
`OLR_GPU_PIPELINE=ON`; (b) an offscreen Metal render smoke runs the shared Metal unit tests on a host
Mac (the same `MTLCreateSystemDefaultDevice` Metal path iOS uses). **Full on-device validation is
manual and documented as such**; the automated CI gate is the iOS build compiling + the shared Metal
unit tests passing on macOS.

**Architecture:** A small `playback/gpu/iosgpupolicy.{h,cpp}` defines the iOS budget/threading policy
in **platform-neutral** code (a query layer, no UIKit), consumed by `gpu-budget`'s cap sizing and by
the worker's render-thread setup. A new `ios/iosgpulifecycle.{h,mm}` is the UIKit-confined seam:
`UIApplication` foreground/background notifications drive a `GpuGenerationCounter::bump()` +
surface-release on suspend and a context-reacquire on resume, mirroring the `device-loss` invalidation
path. The `GpuRhiContext` render-thread contract gains an explicit **"never call UIKit"** invariant
plus a `presentOnMainThread` hook so the rare present/`CAMetalLayer` interaction (used by the preview
path) is marshalled to the main thread via `dispatch_async(dispatch_get_main_queue(), …)`; all
`QRhi`/Metal command encoding stays on the dedicated render thread exactly as on macOS. CMake gains
the iOS GPU-pipeline wiring (the `if(IOS)` block compiles the GPU `.mm` family + the `ios/`
lifecycle source under `OLR_GPU_PIPELINE`, links `Metal`/`QuartzCore`/`UIKit`), and a new
`tests/smoke/check_ios_gpu_build_config.sh` invariant smoke test (the grep-CMake pattern already used
by `check_ios_ffmpeg_build_config.sh`) plus an `iosgpupolicy` unit test that runs in the shared Metal
unit suite on macOS.

**Tech Stack:** C++17, Qt 6 (Core/Gui/GuiPrivate for `QRhi`/`QRhiMetal`, Test), Apple Metal /
CoreVideo / IOSurface (`MTLCreateSystemDefaultDevice`, `MTLSharedEvent`), UIKit
(`UIApplication`, `NSNotificationCenter` foreground/background, `CAMetalLayer` main-thread rule,
`dispatch_get_main_queue`), CMake + Ninja (macOS host) and `qt-cmake -G Xcode` (iOS cross-build).
Consumes the merged Apple GPU edge (`playback/gpu/{gpusurface.h,gpurhicontext.h,gpuframedata.h,
decodedonefence.h,vtkeepsurfaceimporter.h,gpupipelineconfig.h,appleiosurface.h}`), the `gpu-sync`
keystone (`GpuGenerationCounter`, `GpuFence`, `DecodeDoneFence`, the surface-lifetime hooks), the
`gpu-compositor` RHI compositor, the `async-readback` `AsyncGpuReadbackSink`, and the `gpu-budget`
cap-sizing seam.

## Global Constraints

- **Not a new backend — iOS-specific constraints only.** The Apple Metal `.mm` family is the iOS
  implementation. Do **not** fork, duplicate, or re-implement `GpuSurface`/`GpuRhiContext`/
  `GpuFrameData`/`DecodeDoneFence`/`VtKeepSurfaceImporter`. This subproject adds exactly three things:
  an iOS budget/threading **policy**, an iOS GPU **lifecycle** seam, and the **build + smoke** wiring.
  Consume the canonical names verbatim from the merged tree and the `gpu-sync` contract:
  `FrameHandle`, `IFrameData`, `CpuPlanes`, `FrameMetadata`, `FramePayloadKey`, `FramePixelFormat`,
  `ColorMetadata`, `GpuSurface`, `GpuSurfaceDesc`, `GpuRhiContext`, `GpuFrameData`,
  `makeGpuFrameHandle`, `DecodeDoneFence`, `GpuGenerationCounter`, `GpuFence`, `gpuPipelineEnabled`.
  Do not invent variants.
- **CPU path stays default + reference.** The GPU pipeline is two-gated: the `OLR_GPU_PIPELINE` CMake
  option → `OLR_GPU_PIPELINE_BUILD` compile def (CMakeLists.txt:452-453), and the runtime
  `gpuPipelineEnabled()` env flag (off by default). With `OLR_GPU_PIPELINE_BUILD` undefined **or**
  `gpuPipelineEnabled()` false, every byte of this plan is inert: the iOS app runs the CPU path
  exactly as it does today. The iOS budget/lifecycle code is only reached when both gates are on.
- **The iOS main-thread rule is non-negotiable.** UIKit and `CAMetalLayer` present/layout calls must
  run on the main thread; the dedicated `GpuRenderThread` (D11) must never call UIKit. Every iOS GPU
  path either (a) stays on the render thread doing only `QRhi`/Metal command encoding — exactly as on
  macOS — or (b) marshals a UIKit/present touch to `dispatch_get_main_queue()`. There is **no third
  path**. Each lifecycle/present site carries a `// MAIN-THREAD:` or `// RENDER-THREAD:` comment
  proving which thread it runs on.
- **Concurrency-critical — independent review required before merge (CLAUDE.md "Verification").** The
  background/foreground invalidation races the render thread and the worker decode loop. The contract
  is: on suspend, no GPU command references a surface after it is released; on resume, no stale
  (pre-bump generation) surface is presented. The lifecycle seam reuses the `gpu-sync` lock rule
  (collect under the mutex, **release it**, then fence-wait/free — never fence-wait under
  `m_bufferMutex`). The branch gets a fresh-agent concurrency review before the PR merges.
- **iOS validation is honest: build gate + shared Metal unit tests, not on-device.** CI cannot run an
  iOS GPU app on a device or simulator GPU automatically. The automated gates are: the iOS **Xcode
  build compiles** with `OLR_GPU_PIPELINE=ON` (the `tst`-free build-config smoke + the pre-push
  `SKIP_IOS_BUILD` lane), and the **shared Metal unit tests pass on the macOS host** (same
  `MTLCreateSystemDefaultDevice` path). Picture-correctness, thermal behavior, and the
  suspend/resume cycle on a real device are **manual** — each such criterion is labelled
  `MANUAL:` in this plan and never asserted by an automated test.
- **iOS build is opt-in; respect the toolchain gotchas.** The pre-push iOS build is opt-in
  (`SKIP_IOS_BUILD=1` skips it; it auto-skips when the Qt iOS kit is absent). When invoking
  `qt-cmake`/`xcodebuild` from any harness or script, **unset `GIT_CONFIG_*` env vars** (an SPM/
  GIT_CONFIG conflict otherwise breaks the Xcode build). The iOS build path is
  `qt-cmake -S . -B <dir> -G Xcode -DQT_HOST_PATH=$QT_HOST_PREFIX -DCMAKE_OSX_ARCHITECTURES=arm64`
  then `cmake --build <dir> --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO`
  (pre-push:386-391).
- **Tests are not built for the iOS cross-build.** `OLR_BUILD_TESTS AND NOT IOS` (CMakeLists.txt:749)
  — unit/e2e targets link Homebrew FFmpeg and run headless, so they are macOS-host only. The iOS
  contribution to the unit suite is therefore exercised on the **macOS** host build; the iOS target
  only needs to **compile**. Do not add an iOS-only test target.
- **No throwaways; public-repo professionalism.** Every artifact is production and stays in the tree.
  This repo is public: self-contained, professional code/comments/commits; document the present
  design, no internal notes or private history.
- **Format changed lines only** (CI Lint checks changed lines; engine files are hand-Allman):
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h' '*.mm'
  ```
- **Build (run from the worktree root).** Host (macOS) build for the unit tests + smoke, GPU on:
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  iOS cross-build (the compile gate — run with `GIT_CONFIG_*` unset):
  ```sh
  env -u GIT_CONFIG -u GIT_CONFIG_GLOBAL -u GIT_CONFIG_SYSTEM -u GIT_CONFIG_COUNT \
    $HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B build/ios -G Xcode \
    -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DOLR_GPU_PIPELINE=ON
  cmake --build build/ios --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
  ```
  Use a fresh build dir when switching configurations. Unit tests register via
  `olr_add_unit_test(<name> <libs...>)` in `tests/unit/CMakeLists.txt`; GPU tests live inside the
  `if(OLR_GPU_PIPELINE)` block and link `olr_test_gpu`. Qt Test runs headless under
  `QT_QPA_PLATFORM=offscreen`. GPU behavioral tests `QSKIP` where no Metal device exists, never
  hard-fail.

---

## Preconditions (read before Task 1)

- **Phases 0–4 merged.** The merged Apple GPU edge exists: `playback/gpu/{gpusurface.h,
  gpusurface_apple.mm,gpurhicontext.h,gpurhicontext_apple.mm,gpuframedata.{h,cpp},
  decodedonefence.{h,decodedonefence_apple.mm},gpupipelineconfig.{h,cpp},appleiosurface.h,
  applegpusurface_apple.mm,vtkeepsurfaceimporter.{h,vtkeepsurfaceimporter_apple.mm}}`, the
  `OLR_GPU_PIPELINE` CMake block (CMakeLists.txt:452-468), and the e2e GPU counters. The `gpu-sync`
  keystone provides `GpuGenerationCounter` (`playback/gpu/gpugeneration.h`), `GpuFence`
  (`playback/gpu/gpufence.h`), the extended `DecodeDoneFence`, and the `GpuSurface` surface-lifetime
  hooks. `gpu-compositor`, `async-readback`, and `gpu-budget` are merged. Verify with
  `git merge-base --is-ancestor <phase4-sha> origin/main` if unsure. **If `gpu-budget` is not yet
  merged**, Task 2 still lands `iosgpupolicy` (it is self-contained); the wiring into `gpu-budget`'s
  cap sizing is then a one-line follow-up where that cap is computed.
- **The Apple GPU `.mm` family already compiles for iOS arm64.** Every GPU edge file is guarded by a
  single `#ifdef __APPLE__` (e.g. `gpurhicontext_apple.mm:3`, `gpusurface_apple.mm:3`,
  `appleiosurface.h:4`); iOS defines `__APPLE__`, and `QRhi::Metal` + `QRhiMetalInitParams`
  (`gpurhicontext_apple.mm:88-89`) are available in the Qt iOS kit. **Task 1's job is purely the
  CMake wiring** to add these sources to the `if(IOS)` target, not to change the `.mm` bodies. Verify
  the iOS block at CMakeLists.txt:471-478 reuses the same `OpenLiveReplay` target the macOS
  `if(APPLE)` GPU block (455-461) feeds, so adding sources under `if(IOS)` is symmetric.
- **The smoke-test pattern is fixed.** `tests/smoke/check_ios_ffmpeg_build_config.sh` is a
  `set -euo pipefail` bash script with `require_in_file`/`reject_in_file` helpers that grep CMake/
  scripts for invariants, registered in `tests/smoke/CMakeLists.txt` via `add_test(... bash ...)` with
  `set_tests_properties(... PROPERTIES LABELS "smoke;ci" TIMEOUT 10)`. Task 4 follows this exact
  pattern for the iOS GPU build-config smoke.
- **`gpuPipelineEnabled()` reads `OLR_GPU_PIPELINE` at runtime** (`gpupipelineconfig.h:6`,
  off by default) and is **independent of the `OLR_GPU_PIPELINE_BUILD` compile def**. On iOS there is
  no shell env in the normal launch flow, so the iOS budget/lifecycle code must be reachable through
  the same `gpuPipelineEnabled()` gate (the gate also honors a future Info.plist/UserDefaults source
  if added — out of scope here). This plan does not change how the gate is read.

---

## File Structure

- **Create** `playback/gpu/iosgpupolicy.h`, `playback/gpu/iosgpupolicy.cpp` — platform-neutral iOS
  GPU policy (no UIKit): `gpuIsIosBuild()`, the iOS-aggressive per-track GPU-window cap, and the
  render-thread "never call UIKit" invariant documented as a compile-time-checkable constant. Consumed
  by `gpu-budget` cap sizing and the worker render-thread setup.
- **Create** `ios/iosgpulifecycle.h`, `ios/iosgpulifecycle.mm` — UIKit-confined background/foreground
  seam: subscribes to `UIApplicationDidEnterBackgroundNotification` /
  `UIApplicationWillEnterForegroundNotification`, calls into the platform-neutral
  `IosGpuLifecycleSink` (release on suspend / reacquire on resume), bumping `GpuGenerationCounter`.
- **Create** `playback/gpu/iosgpulifecyclesink.h`, `playback/gpu/iosgpulifecyclesink.cpp` —
  platform-neutral `IosGpuLifecycleSink` interface + a default impl that bumps
  `GpuGenerationCounter` and invalidates GPU surfaces via the `device-loss`-style path. Lets the
  lifecycle logic be unit-tested on the macOS host with no UIKit.
- **Modify** `playback/gpu/gpurhicontext.h` / `gpurhicontext_apple.mm` — add a `presentOnMainThread`
  marshalling hook (`dispatch_get_main_queue` on iOS, direct on macOS) + the explicit
  render-thread/UIKit invariant comment. No change to the existing `importAndReadback` contract.
- **Modify** `CMakeLists.txt` — under `if(IOS)`, compile the GPU `.mm` family + `iosgpupolicy.cpp` +
  `iosgpulifecyclesink.cpp` + `ios/iosgpulifecycle.mm` when `OLR_GPU_PIPELINE`; link
  `Metal`/`QuartzCore`/`UIKit` and `Qt6::GuiPrivate`; define `OLR_GPU_PIPELINE_BUILD=1`. Compile
  `iosgpupolicy.cpp` + `iosgpulifecyclesink.cpp` on macOS/host too (they are platform-neutral).
- **Modify** `tests/CMakeLists.txt` — add `iosgpupolicy.cpp` + `iosgpulifecyclesink.cpp` to
  `olr_test_gpu` so the iOS policy/lifecycle unit tests link on the macOS host.
- **Modify** `tests/unit/CMakeLists.txt` — register `tst_iosgpupolicy` and `tst_iosgpulifecycle` in
  the `if(OLR_GPU_PIPELINE)` block linking `olr_test_gpu`.
- **Create** `tests/smoke/check_ios_gpu_build_config.sh` — invariant smoke (grep CMake) asserting the
  iOS GPU wiring is present, registered in `tests/smoke/CMakeLists.txt` with `LABELS "smoke;ci"`.
- **Create** tests: `tests/unit/tst_iosgpupolicy.cpp`, `tests/unit/tst_iosgpulifecycle.cpp`.

---

## Task 1: iOS GPU-pipeline CMake wiring (the Apple `.mm` family compiles for iOS arm64)

**Precondition:** Phases 0–4 merged. **No `.mm` body changes** — pure build wiring.

**Files:**
- Modify: `CMakeLists.txt`
- Test: `tests/smoke/check_ios_gpu_build_config.sh` (created in Task 4; here we only add the CMake
  lines those invariants will assert — Task 4 adds the asserting script).

**Interfaces:**
- Consumes: the merged Apple GPU sources (`playback/gpu/gpurhicontext_apple.mm`,
  `gpuframedata.cpp`, `decodedonefence_apple.mm`, `vtkeepsurfaceimporter_apple.mm`, and the
  already-iOS-linked `gpusurface_apple.mm`/`applegpusurface_apple.mm`/`gpupipelineconfig.cpp`).
- Produces: an iOS `OpenLiveReplay` target that builds with `-DOLR_GPU_PIPELINE=ON`.

- [ ] **Step 1: Add the failing build gate (iOS configure with GPU on must currently fail to link/compile the GPU edge)**

From the worktree root, configure the iOS target with the GPU pipeline on to capture the
pre-implementation state:

```sh
env -u GIT_CONFIG -u GIT_CONFIG_GLOBAL -u GIT_CONFIG_SYSTEM -u GIT_CONFIG_COUNT \
  $HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B build/ios -G Xcode \
  -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DCMAKE_OSX_ARCHITECTURES=arm64 -DOLR_GPU_PIPELINE=ON
cmake --build build/ios --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
```

Expected: FAIL — the `if(IOS)` block does not compile the GPU `.mm` family (the GPU sources are added
only under the macOS `if(APPLE)` GPU block at CMakeLists.txt:455-461, which is **not** the `if(IOS)`
branch), so either the GPU types are missing at link or `OLR_GPU_PIPELINE_BUILD` is undefined for the
iOS target. (If the host has no Qt iOS kit, the gate cannot run here; proceed to Step 3 and use the
Task-4 build-config smoke as the standing gate. Document the skip.)

- [ ] **Step 2: Confirm the off-path iOS build is unaffected (baseline green)**

```sh
env -u GIT_CONFIG -u GIT_CONFIG_GLOBAL -u GIT_CONFIG_SYSTEM -u GIT_CONFIG_COUNT \
  $HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B build/ios-off -G Xcode \
  -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/ios-off --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
```

Expected: PASS (the default iOS build, GPU pipeline off, is the shipping iOS app today).

- [ ] **Step 3: Write the minimal implementation (extend the `if(OLR_GPU_PIPELINE)` block to cover iOS)**

In `CMakeLists.txt`, the GPU-pipeline block (lines 452-468) currently branches `if(APPLE) … else()`.
On iOS both `APPLE` and `IOS` are true, so the `if(APPLE)` arm is *taken* — but the iOS target needs
`UIKit` for the lifecycle seam and must also compile `ios/iosgpulifecycle.mm` + the new policy/sink
sources. Replace the GPU block's Apple arm body so it adds the iOS-only lifecycle source and links
`UIKit` when `IOS`, keeping the macOS path identical:

```cmake
if(OLR_GPU_PIPELINE)
    target_compile_definitions(OpenLiveReplay PRIVATE OLR_GPU_PIPELINE_BUILD=1)
    target_link_libraries(OpenLiveReplay PRIVATE Qt6::GuiPrivate)
    if(APPLE)
        target_sources(OpenLiveReplay PRIVATE
            playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm
            playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp
            playback/gpu/decodedonefence.h playback/gpu/decodedonefence_apple.mm
            playback/gpu/vtkeepsurfaceimporter.h playback/gpu/vtkeepsurfaceimporter_apple.mm
            playback/gpu/iosgpupolicy.h playback/gpu/iosgpupolicy.cpp
            playback/gpu/iosgpulifecyclesink.h playback/gpu/iosgpulifecyclesink.cpp)
        target_link_libraries(OpenLiveReplay PRIVATE "-framework Metal" "-framework QuartzCore")
        if(IOS)
            target_sources(OpenLiveReplay PRIVATE
                ios/iosgpulifecycle.h ios/iosgpulifecycle.mm)
            target_link_libraries(OpenLiveReplay PRIVATE "-framework UIKit")
        endif()
    else()
        target_sources(OpenLiveReplay PRIVATE
            playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_stub.cpp
            playback/gpu/gpuframedata.h playback/gpu/gpuframedata.cpp
            playback/gpu/decodedonefence.h playback/gpu/decodedonefence_stub.cpp
            playback/gpu/iosgpupolicy.h playback/gpu/iosgpupolicy.cpp
            playback/gpu/iosgpulifecyclesink.h playback/gpu/iosgpulifecyclesink.cpp)
    endif()
endif()
```

> `iosgpupolicy.{h,cpp}` and `iosgpulifecyclesink.{h,cpp}` are created in Tasks 2-3; for Task 1, create
> them as empty-but-compilable stubs (a header guard + a one-line `.cpp` including the header) so the
> source list resolves. Tasks 2-3 fill the bodies via TDD. The `ios/iosgpulifecycle.{h,mm}` are
> created in Task 3.

Create the four stub sources now so Task 1 builds:

`playback/gpu/iosgpupolicy.h`:
```cpp
#ifndef OLR_IOSGPUPOLICY_H
#define OLR_IOSGPUPOLICY_H

// iOS GPU-pipeline policy (platform-neutral; no UIKit). Filled in Task 2.
bool gpuIsIosBuild();

#endif // OLR_IOSGPUPOLICY_H
```

`playback/gpu/iosgpupolicy.cpp`:
```cpp
#include "playback/gpu/iosgpupolicy.h"

bool gpuIsIosBuild() {
#if defined(__APPLE__) && TARGET_OS_IOS
    return true;
#else
    return false;
#endif
}
```

> `TARGET_OS_IOS` comes from `<TargetConditionals.h>`; include it at the top of the `.cpp` (`#include
> <TargetConditionals.h>` under `#ifdef __APPLE__`). Task 2 replaces this body with the tested version.

`playback/gpu/iosgpulifecyclesink.h`:
```cpp
#ifndef OLR_IOSGPULIFECYCLESINK_H
#define OLR_IOSGPULIFECYCLESINK_H

// iOS background/foreground GPU lifecycle sink (platform-neutral). Filled in Task 3.
class IosGpuLifecycleSink {
public:
    virtual ~IosGpuLifecycleSink();
    virtual void onEnterBackground() = 0;
    virtual void onEnterForeground() = 0;
};

#endif // OLR_IOSGPULIFECYCLESINK_H
```

`playback/gpu/iosgpulifecyclesink.cpp`:
```cpp
#include "playback/gpu/iosgpulifecyclesink.h"

IosGpuLifecycleSink::~IosGpuLifecycleSink() = default;
```

- [ ] **Step 4: Run the iOS build gate, expect PASS**

```sh
cmake --build build/ios --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
```

Expected: PASS — the iOS target now compiles the GPU edge. (If no Qt iOS kit on the host, this is the
Task-4 smoke's job; record the skip and rely on the pre-push `SKIP_IOS_BUILD` lane.)

- [ ] **Step 5: Verify the host zero-regression gate**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
```

Expected: the full host unit suite PASSES unchanged (the new sources are additive; nothing consumes
the policy/sink yet).

- [ ] **Step 6: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add CMakeLists.txt playback/gpu/iosgpupolicy.h playback/gpu/iosgpupolicy.cpp \
        playback/gpu/iosgpulifecyclesink.h playback/gpu/iosgpulifecyclesink.cpp
git commit -m "feat(ios-bringup): compile the GPU Metal edge for the iOS arm64 target"
```

---

## Task 2: iOS GPU-window budget policy (more aggressive than macOS)

**Precondition:** Task 1. `gpu-budget` merged (else land the policy standalone; wire later).

**Files:**
- Modify: `playback/gpu/iosgpupolicy.h`, `playback/gpu/iosgpupolicy.cpp`
- Test: `tests/unit/tst_iosgpupolicy.cpp`
- Modify: `tests/CMakeLists.txt` (add the policy source to `olr_test_gpu`), `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing platform-specific (pure policy).
- Produces:
  ```cpp
  // playback/gpu/iosgpupolicy.h — platform-neutral; NO UIKit/Metal types.
  // True when compiled for the iOS target (TARGET_OS_IOS), false on macOS/host.
  bool gpuIsIosBuild();

  // The per-track GPU decode-window cap for the current platform, given the
  // active track count. iOS returns a SMALLER cap than macOS (thermal + VRAM):
  // macOS keeps the existing window-derived cap (max(12, 256/trackCount), spec
  // §2); iOS clamps it to kIosMaxPerTrackGpuFrames so the aggregate GPU window is
  // bounded well below the macOS 256-surface ceiling. gpu-budget calls this when
  // sizing the GPU window; with the GPU pipeline off the CPU cap is unchanged.
  int gpuPerTrackWindowCap(int trackCount);

  // The documented iOS aggregate-window ceiling (surfaces), used by gpu-budget's
  // OOM-degrade sizing. macOS uses the spec §2 peak formula; iOS uses this floor.
  int gpuIosAggregateWindowCeiling();

  // The iOS per-track ceiling constant (documented budget; thermal/VRAM bound).
  constexpr int kIosMaxPerTrackGpuFrames = 8;
  // The iOS aggregate ceiling constant (documented budget).
  constexpr int kIosAggregateGpuFrameCeiling = 48;
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_iosgpupolicy.cpp`:

```cpp
// The iOS GPU budget is deliberately more aggressive than macOS (thermal + VRAM).
// This test pins the documented caps and proves the iOS clamp is strictly below
// the macOS window-derived cap for the same track count. It runs on the macOS
// host; gpuIsIosBuild() is false there, so the host branch is exercised, and the
// iOS branch is exercised via the explicit cap helpers (which take the platform
// as the compile-time TARGET_OS_IOS — here we assert the constants + the clamp
// math directly so the iOS values are gated regardless of host).
#include <QtTest>

#include "playback/gpu/iosgpupolicy.h"

class TestIosGpuPolicy : public QObject {
    Q_OBJECT
private slots:
    void hostIsNotIosBuild();
    void iosCapsAreDocumentedAndTight();
    void perTrackCapNeverExceedsIosCeilingOnIos();
};

void TestIosGpuPolicy::hostIsNotIosBuild() {
    // The macOS host build is not an iOS build.
    QVERIFY(!gpuIsIosBuild());
}

void TestIosGpuPolicy::iosCapsAreDocumentedAndTight() {
    // The iOS budget constants are the documented thermal/VRAM bound and are
    // strictly tighter than the macOS 256-surface aggregate / 12-floor per-track.
    QVERIFY(kIosMaxPerTrackGpuFrames > 0);
    QVERIFY(kIosMaxPerTrackGpuFrames < 12);          // below the macOS per-track floor
    QVERIFY(kIosAggregateGpuFrameCeiling < 256);     // below the macOS aggregate ceiling
    QCOMPARE(gpuIosAggregateWindowCeiling(), kIosAggregateGpuFrameCeiling);
}

void TestIosGpuPolicy::perTrackCapNeverExceedsIosCeilingOnIos() {
    // For any track count, the iOS per-track cap is clamped to the ceiling. We
    // exercise the clamp by asserting the helper's monotonic clamp behavior: with
    // few tracks the macOS formula would give a large cap; the iOS path must not.
    // On the host (non-iOS) the cap follows the macOS formula; that is asserted by
    // the >= ceiling check only holding on iOS. Here we pin the clamp helper's
    // invariant directly: the returned cap is never above the ceiling when on iOS.
    if (!gpuIsIosBuild()) QSKIP("iOS clamp branch only active on the iOS target");
    for (int n = 1; n <= 8; ++n)
        QVERIFY(gpuPerTrackWindowCap(n) <= kIosMaxPerTrackGpuFrames);
}

QTEST_GUILESS_MAIN(TestIosGpuPolicy)
#include "tst_iosgpupolicy.moc"
```

Register in `tests/unit/CMakeLists.txt`, inside the `if(OLR_GPU_PIPELINE)` block alongside the other
GPU unit tests:

```cmake
olr_add_unit_test(tst_iosgpupolicy olr_test_gpu)
```

Add the policy source to `olr_test_gpu` in `tests/CMakeLists.txt` (the `qt_add_library(olr_test_gpu …)`
list at lines 288-292):

```cmake
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosgpupolicy.cpp"
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
cmake --build build/c --target tst_iosgpupolicy
```

Expected: FAIL to compile — `gpuPerTrackWindowCap` / `gpuIosAggregateWindowCeiling` /
`kIosMaxPerTrackGpuFrames` undeclared (the Task-1 stub header has only `gpuIsIosBuild`).

- [ ] **Step 3: Write the implementation**

Replace `playback/gpu/iosgpupolicy.h` with the full interface:

```cpp
#ifndef OLR_IOSGPUPOLICY_H
#define OLR_IOSGPUPOLICY_H

// iOS GPU-pipeline policy. PLATFORM-NEUTRAL: no UIKit/Metal/CoreVideo types — this
// header is included by gpu-budget cap-sizing and the worker render-thread setup on
// every platform. The iOS budget is deliberately TIGHTER than macOS: iOS pays a
// thermal-throttling + VRAM cost macOS (unified memory, mains power) does not, so
// the GPU decode window is bounded well below the macOS 256-surface aggregate.

// The iOS per-track GPU decode-window ceiling (surfaces). Documented thermal/VRAM
// budget — kept small so the aggregate window cannot blow VRAM under multiview.
constexpr int kIosMaxPerTrackGpuFrames = 8;
// The iOS aggregate GPU decode-window ceiling (surfaces) across all feeds.
constexpr int kIosAggregateGpuFrameCeiling = 48;

// True when compiled for the iOS target (TARGET_OS_IOS), false on macOS/host.
bool gpuIsIosBuild();

// Per-track GPU decode-window cap given the active track count. macOS keeps the
// existing window-derived cap (max(12, 256/trackCount), spec §2). iOS clamps that
// to kIosMaxPerTrackGpuFrames. gpu-budget calls this when sizing the GPU window.
int gpuPerTrackWindowCap(int trackCount);

// The platform aggregate GPU decode-window ceiling (surfaces). macOS: 256 (spec
// §2 peak formula upstream). iOS: kIosAggregateGpuFrameCeiling.
int gpuIosAggregateWindowCeiling();

#endif // OLR_IOSGPUPOLICY_H
```

Replace `playback/gpu/iosgpupolicy.cpp`:

```cpp
#include "playback/gpu/iosgpupolicy.h"

#include <algorithm>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace {
// The macOS window-derived per-track cap (spec §2): max(12, 256/trackCount).
int macosPerTrackCap(int trackCount) {
    const int n = trackCount > 0 ? trackCount : 1;
    return std::max(12, 256 / n);
}
} // namespace

bool gpuIsIosBuild() {
#if defined(__APPLE__) && TARGET_OS_IOS
    return true;
#else
    return false;
#endif
}

int gpuPerTrackWindowCap(int trackCount) {
    const int base = macosPerTrackCap(trackCount);
    if (gpuIsIosBuild()) return std::min(base, kIosMaxPerTrackGpuFrames);
    return base;
}

int gpuIosAggregateWindowCeiling() {
    return gpuIsIosBuild() ? kIosAggregateGpuFrameCeiling : 256;
}
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_iosgpupolicy && \
  ctest --test-dir build/c -R tst_iosgpupolicy --output-on-failure
```

Expected: PASS (3 tests on the macOS host; the iOS-only clamp slot `QSKIP`s on the host, the
constants + ceiling slots run).

- [ ] **Step 5: Wire the cap into `gpu-budget` (only if `gpu-budget` is merged)**

Find the `gpu-budget` GPU-window cap site (where the per-track GPU surface cap is computed for the
GPU path — analogous to the CPU `capFrames` clamp in `playback/playbackworker.cpp:169`). Route the
GPU-path cap through `gpuPerTrackWindowCap(trackCount)` so iOS uses the tight budget while macOS is
unchanged. Add a one-line comment citing the iOS thermal/VRAM rationale. **If `gpu-budget` is not yet
merged**, skip this step; the policy stands alone and the wiring is the follow-up.

- [ ] **Step 6: Zero-regression + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/gpu/iosgpupolicy.h playback/gpu/iosgpupolicy.cpp \
        tests/unit/tst_iosgpupolicy.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(ios-bringup): tighter iOS GPU decode-window budget (thermal + VRAM)"
```

---

## Task 3: Background/foreground GPU lifecycle seam (release/reacquire, generation bump)

**Precondition:** Task 2. `gpu-sync` merged (`GpuGenerationCounter`); `device-loss` merged for the
invalidation path (else the sink bumps the generation and degrades, which is the floor).

**Files:**
- Create: `ios/iosgpulifecycle.h`, `ios/iosgpulifecycle.mm`
- Modify: `playback/gpu/iosgpulifecyclesink.h`, `playback/gpu/iosgpulifecyclesink.cpp`
- Test: `tests/unit/tst_iosgpulifecycle.cpp`
- Modify: `tests/CMakeLists.txt` (add the sink source to `olr_test_gpu`), `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuGenerationCounter` (`playback/gpu/gpugeneration.h`, `instance()`, `current()`,
  `bump()`, `reset()`).
- Produces:
  ```cpp
  // playback/gpu/iosgpulifecyclesink.h — platform-neutral; NO UIKit types.
  // The GPU lifecycle reaction to iOS suspend/resume. Default impl bumps the GPU
  // generation (invalidating in-flight surfaces, reusing the device-loss seam) on
  // background, and marks the context for reacquire on foreground.
  class IosGpuLifecycleSink {
  public:
      virtual ~IosGpuLifecycleSink();
      virtual void onEnterBackground() = 0;   // release GPU resources, bump generation
      virtual void onEnterForeground() = 0;    // reacquire on next decode/render
  };

  // The default sink: bumps GpuGenerationCounter on background (so every surface
  // minted in the suspended generation is stale on resume) and clears the suspend
  // flag on foreground. Surface-release/reacquire is driven by the generation
  // bump exactly like device-loss; no UIKit here.
  class DefaultIosGpuLifecycleSink : public IosGpuLifecycleSink {
  public:
      void onEnterBackground() override;
      void onEnterForeground() override;
      bool isSuspended() const;                // test observation
      uint64_t generationAtLastBackground() const;
  private:
      bool m_suspended = false;
      uint64_t m_bgGeneration = 0;
  };

  // ios/iosgpulifecycle.h — UIKit-confined registrar (declared C-callable so the
  // Qt/UIKit entry can install it without pulling ObjC into C++ TUs).
  #ifdef __cplusplus
  extern "C" {
  #endif
  // Subscribe the process to UIApplication background/foreground notifications,
  // routing them to the registered sink. Idempotent; no-op if already installed.
  // MUST be called on the main thread (UIKit notification registration).
  void installIosGpuLifecycle(void);
  #ifdef __cplusplus
  }
  #endif
  // C++-only: set the sink the UIKit registrar forwards to (defaults to a process
  // DefaultIosGpuLifecycleSink). Lets the worker inject a sink that knows the
  // GpuRhiContext. Thread-safe (atomic pointer swap).
  void setIosGpuLifecycleSink(IosGpuLifecycleSink* sink);
  IosGpuLifecycleSink* iosGpuLifecycleSink();
  ```

- [ ] **Step 1: Write the failing test (the platform-neutral sink, host-runnable)**

Create `tests/unit/tst_iosgpulifecycle.cpp`:

```cpp
// The iOS GPU lifecycle sink is the platform-neutral reaction to suspend/resume.
// It is unit-tested on the macOS host with NO UIKit: onEnterBackground() bumps the
// GPU generation (so suspended-generation surfaces are stale on resume — the
// device-loss invalidation seam) and sets a suspend flag; onEnterForeground()
// clears it. This proves the invalidation contract without a device.
#include <QtTest>

#include "playback/gpu/iosgpulifecyclesink.h"
#include "playback/gpu/iosgpugeneration_test_shim.h"  // see note below

class TestIosGpuLifecycle : public QObject {
    Q_OBJECT
private slots:
    void init();
    void backgroundBumpsGenerationAndSuspends();
    void foregroundClearsSuspend();
    void sinkRegistryRoundTrips();
};

void TestIosGpuLifecycle::init() {
    GpuGenerationCounter::instance().reset();
}

void TestIosGpuLifecycle::backgroundBumpsGenerationAndSuspends() {
    DefaultIosGpuLifecycleSink sink;
    const uint64_t before = GpuGenerationCounter::instance().current();
    QVERIFY(!sink.isSuspended());
    sink.onEnterBackground();
    QVERIFY(sink.isSuspended());
    // The generation advanced: any surface minted in `before` is now stale.
    QVERIFY(GpuGenerationCounter::instance().current() > before);
    QCOMPARE(sink.generationAtLastBackground(), GpuGenerationCounter::instance().current());
}

void TestIosGpuLifecycle::foregroundClearsSuspend() {
    DefaultIosGpuLifecycleSink sink;
    sink.onEnterBackground();
    QVERIFY(sink.isSuspended());
    sink.onEnterForeground();
    QVERIFY(!sink.isSuspended());
}

void TestIosGpuLifecycle::sinkRegistryRoundTrips() {
    DefaultIosGpuLifecycleSink sink;
    setIosGpuLifecycleSink(&sink);
    QCOMPARE(iosGpuLifecycleSink(), static_cast<IosGpuLifecycleSink*>(&sink));
    setIosGpuLifecycleSink(nullptr);
    QVERIFY(iosGpuLifecycleSink() != &sink);  // falls back to the process default
}

QTEST_GUILESS_MAIN(TestIosGpuLifecycle)
#include "tst_iosgpulifecycle.moc"
```

> The test includes `gpugeneration.h` for `GpuGenerationCounter`; replace the placeholder
> `iosgpugeneration_test_shim.h` include with the real `#include "playback/gpu/gpugeneration.h"`. (The
> shim line above is a reminder to point at the merged `gpu-sync` header, not to create a new file.)

Register in `tests/unit/CMakeLists.txt`, in the `if(OLR_GPU_PIPELINE)` block:

```cmake
olr_add_unit_test(tst_iosgpulifecycle olr_test_gpu)
```

Add the sink source to `olr_test_gpu` in `tests/CMakeLists.txt` (it does not pull UIKit, so it links
on the host):

```cmake
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosgpulifecyclesink.cpp"
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_iosgpulifecycle
```

Expected: FAIL to compile — `DefaultIosGpuLifecycleSink` / `setIosGpuLifecycleSink` /
`iosGpuLifecycleSink` undeclared (the Task-1 stub header has only the abstract base).

- [ ] **Step 3: Write the platform-neutral sink implementation**

Replace `playback/gpu/iosgpulifecyclesink.h` with the full interface (the `DefaultIosGpuLifecycleSink`
+ the registry declarations above). Replace `playback/gpu/iosgpulifecyclesink.cpp`:

```cpp
#include "playback/gpu/iosgpulifecyclesink.h"

#include "playback/gpu/gpugeneration.h"

#include <atomic>

IosGpuLifecycleSink::~IosGpuLifecycleSink() = default;

void DefaultIosGpuLifecycleSink::onEnterBackground() {
    // RELEASE: bump the GPU generation so every surface minted before suspend is
    // stale on resume — the same invalidation seam device-loss uses. The actual
    // surface free happens lazily as stale handles drop (the gpu-sync eviction
    // guard already honors in-flight fences; the lock rule applies upstream).
    m_bgGeneration = GpuGenerationCounter::instance().bump();
    m_suspended = true;
}

void DefaultIosGpuLifecycleSink::onEnterForeground() {
    // REACQUIRE: clear the suspend flag; the next decode/render mints surfaces in
    // the new generation against a (re)valid context. No UIKit here.
    m_suspended = false;
}

bool DefaultIosGpuLifecycleSink::isSuspended() const { return m_suspended; }
uint64_t DefaultIosGpuLifecycleSink::generationAtLastBackground() const { return m_bgGeneration; }

namespace {
DefaultIosGpuLifecycleSink g_defaultSink;
std::atomic<IosGpuLifecycleSink*> g_sink{&g_defaultSink};
} // namespace

void setIosGpuLifecycleSink(IosGpuLifecycleSink* sink) {
    g_sink.store(sink ? sink : static_cast<IosGpuLifecycleSink*>(&g_defaultSink),
                 std::memory_order_release);
}

IosGpuLifecycleSink* iosGpuLifecycleSink() {
    return g_sink.load(std::memory_order_acquire);
}
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_iosgpulifecycle && \
  ctest --test-dir build/c -R tst_iosgpulifecycle --output-on-failure
```

Expected: PASS (4 tests on the host — the sink contract is fully exercised without UIKit).

- [ ] **Step 5: Add the UIKit registrar (`ios/iosgpulifecycle.mm`) — iOS-only, build-gated**

Create `ios/iosgpulifecycle.h` (the `extern "C" installIosGpuLifecycle` + the sink registry decls
above). Create `ios/iosgpulifecycle.mm`:

```objcpp
#import "ios/iosgpulifecycle.h"

#ifdef __APPLE__
#import <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS

#import <UIKit/UIKit.h>

#include "playback/gpu/iosgpulifecyclesink.h"

#include <atomic>

namespace {
std::atomic<bool> g_installed{false};
id g_bgObserver = nil;
id g_fgObserver = nil;
} // namespace

extern "C" void installIosGpuLifecycle(void) {
    // MAIN-THREAD: UIKit notification registration must run on the main thread.
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;  // idempotent

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    g_bgObserver = [nc addObserverForName:UIApplicationDidEnterBackgroundNotification
                                   object:nil
                                    queue:[NSOperationQueue mainQueue]
                               usingBlock:^(NSNotification*) {
                                   // MAIN-THREAD: forward to the platform-neutral sink.
                                   if (auto* s = iosGpuLifecycleSink()) s->onEnterBackground();
                               }];
    g_fgObserver = [nc addObserverForName:UIApplicationWillEnterForegroundNotification
                                   object:nil
                                    queue:[NSOperationQueue mainQueue]
                               usingBlock:^(NSNotification*) {
                                   // MAIN-THREAD: forward to the platform-neutral sink.
                                   if (auto* s = iosGpuLifecycleSink()) s->onEnterForeground();
                               }];
}

#else  // not iOS — the registrar is a no-op so non-iOS TUs link cleanly.

extern "C" void installIosGpuLifecycle(void) {}

#endif
```

> `ios/iosgpulifecycle.mm` is compiled only under the `if(IOS)` GPU block (Task 1 already added it
> there). It is **not** added to `olr_test_gpu` (it pulls UIKit); the host test exercises the sink, not
> the registrar. The registrar is covered by the iOS build gate (Task 4) compiling it.

- [ ] **Step 6: Call `installIosGpuLifecycle()` from the existing iOS entry (reuse `ios/ios_scene`)**

The lifecycle must be installed once on launch. The existing `ios/ios_scene.mm` is the iOS-specific
entry seam. Add an install hook there (or at the app's Qt/UIKit bootstrap) guarded by
`gpuPipelineEnabled()` so it is inert with the GPU pipeline off:

```objcpp
// In the iOS launch path (ios/ios_scene.mm or the app bootstrap), once UIKit is up:
//   #include "playback/gpu/gpupipelineconfig.h"
//   #import "ios/iosgpulifecycle.h"
//   if (gpuPipelineEnabled()) installIosGpuLifecycle();  // MAIN-THREAD
```

Keep this call on the main thread (it is the UIKit bootstrap thread). **MANUAL:** the actual
suspend→resume→still-renders behavior is validated on-device, not in CI.

- [ ] **Step 7: Zero-regression + iOS build gate + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
# iOS compile gate (or rely on Task-4 smoke + pre-push SKIP_IOS_BUILD lane):
cmake --build build/ios --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/iosgpulifecyclesink.h playback/gpu/iosgpulifecyclesink.cpp \
        ios/iosgpulifecycle.h ios/iosgpulifecycle.mm ios/ios_scene.mm \
        tests/unit/tst_iosgpulifecycle.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(ios-bringup): background/foreground GPU release+reacquire via generation bump"
```

**Review gate:** the suspend/resume invalidation races the render thread + decode loop. Carry the
`gpu-sync` lock rule (collect under `m_bufferMutex`, release, then fence-wait/free) at the surface-free
site this generation bump triggers. Request a fresh-agent concurrency review before merge.

---

## Task 4: Render-thread / iOS main-thread affinity + `presentOnMainThread` marshalling

**Precondition:** Task 3.

**Files:**
- Modify: `playback/gpu/gpurhicontext.h`, `playback/gpu/gpurhicontext_apple.mm`
- Test: extend `tests/unit/tst_iosgpulifecycle.cpp` (same exe — the marshalling contract is testable
  host-side via a thread-id capture)

**Interfaces:**
- Consumes: `GpuRhiContext` (existing — `create()`, `isValid()`, `importAndReadback()`).
- Produces (ADD to `GpuRhiContext`, keep existing methods):
  ```cpp
  // playback/gpu/gpurhicontext.h
  class GpuRhiContext {
  public:
      static std::shared_ptr<GpuRhiContext> create();
      ~GpuRhiContext();
      bool isValid() const;
      CpuPlanes importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                  FramePixelFormat target);   // existing — render thread

      // NEW: run a UIKit/CAMetalLayer-touching present block. On iOS this is
      // marshalled to dispatch_get_main_queue() (the main-thread present rule); on
      // macOS it runs inline. The render thread (importAndReadback) NEVER calls
      // UIKit — this is the single sanctioned UIKit-touch chokepoint. The block is
      // run synchronously (the caller blocks until it has run on the main thread).
      void presentOnMainThread(const std::function<void()>& block);
  };
  ```

- [ ] **Step 1: Write the failing test (present block runs; on the host it runs inline)**

Add to `tests/unit/tst_iosgpulifecycle.cpp`:

```cpp
    void presentBlockRunsOnceOnHost();
```

```cpp
void TestIosGpuLifecycle::presentBlockRunsOnceOnHost() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");
    int ran = 0;
    // On the macOS host (non-iOS) the present block runs inline on the caller
    // thread exactly once. On iOS it would marshal to the main queue; that path is
    // build-gated and validated on-device (MANUAL).
    ctx->presentOnMainThread([&] { ++ran; });
    QCOMPARE(ran, 1);
}
```

Add the include at the top of the test: `#include "playback/gpu/gpurhicontext.h"`.

> Link the test against the real `GpuRhiContext`: on the host, `olr_test_gpu` already compiles
> `gpurhicontext_apple.mm` (Apple) / `gpurhicontext_stub.cpp` (non-Apple). Add `presentOnMainThread`
> to the stub too (Step 3) so the non-Apple host links.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_iosgpulifecycle
```

Expected: FAIL to compile — `GpuRhiContext::presentOnMainThread` undeclared.

- [ ] **Step 3: Write the implementation**

Add the declaration to `playback/gpu/gpurhicontext.h` (after `importAndReadback`):

```cpp
    // Run a UIKit/CAMetalLayer-touching present block on the platform's present
    // thread. iOS marshals to the main thread (dispatch_get_main_queue); macOS runs
    // it inline. The dedicated render thread (importAndReadback) NEVER calls UIKit —
    // this is the single sanctioned UIKit chokepoint (the iOS main-thread rule).
    // Synchronous: the caller blocks until the block has run.
    void presentOnMainThread(const std::function<void()>& block);
```

Add `#include <functional>` to the header if not already present.

In `playback/gpu/gpurhicontext_apple.mm`, implement it. On iOS, dispatch to the main queue
synchronously (and short-circuit if already on the main thread to avoid a deadlock); on macOS run
inline:

```objcpp
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>

void GpuRhiContext::presentOnMainThread(const std::function<void()>& block) {
    if (!block) return;
#if TARGET_OS_IOS
    // MAIN-THREAD: iOS requires UIKit/CAMetalLayer present on the main thread.
    if ([NSThread isMainThread]) {
        block();  // already on main; running inline avoids dispatch_sync deadlock
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ block(); });
    }
#else
    // macOS: no UIKit main-thread present rule for the offscreen pipeline — inline.
    block();
#endif
}
```

> `gpurhicontext_apple.mm` already imports Metal/CoreVideo; add `#import <Foundation/Foundation.h>`
> for `NSThread` if not transitively available. Add the same method to
> `playback/gpu/gpurhicontext_stub.cpp` (non-Apple host/Linux/Windows) running the block inline so the
> stub build links:
> ```cpp
> void GpuRhiContext::presentOnMainThread(const std::function<void()>& block) {
>     if (block) block();
> }
> ```

Also add the **render-thread/UIKit invariant comment** at the top of the `GpuRenderThread` class in
`gpurhicontext_apple.mm` (the dedicated render thread, currently at lines 83-151):

```objcpp
// RENDER-THREAD INVARIANT (iOS main-thread rule): this thread runs ONLY QRhi/Metal
// command encoding (beginOffscreenFrame/endOffscreenFrame, import, readback). It
// MUST NOT call UIKit or touch a CAMetalLayer. Any present/UIKit interaction is
// marshalled to the main thread via GpuRhiContext::presentOnMainThread.
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_iosgpulifecycle && \
  ctest --test-dir build/c -R tst_iosgpulifecycle --output-on-failure
```

Expected: PASS (5 tests; the present-block slot `QSKIP`s if no Metal device, else runs inline once).

- [ ] **Step 5: Zero-regression + iOS build gate + commit**

```sh
cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure
cmake --build build/ios --config Debug -- CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h' '*.mm'
git add playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm \
        playback/gpu/gpurhicontext_stub.cpp tests/unit/tst_iosgpulifecycle.cpp
git commit -m "feat(ios-bringup): main-thread present marshalling + render-thread UIKit invariant"
```

**Review gate:** `presentOnMainThread` uses `dispatch_sync` to the main queue — a classic
deadlock vector if a render-thread holds a lock the main thread also needs. The same-thread
short-circuit guards the trivial case; request a fresh-agent concurrency review of the call sites
before merge.

---

## Task 5: iOS GPU build-config smoke test (CI gate: the wiring is present)

**Precondition:** Tasks 1-4.

**Files:**
- Create: `tests/smoke/check_ios_gpu_build_config.sh`
- Modify: `tests/smoke/CMakeLists.txt`

**Interfaces:**
- Consumes: `CMakeLists.txt` (the iOS GPU wiring from Task 1).
- Produces: a `smoke;ci`-labelled ctest that fails if the iOS GPU wiring regresses — the standing CI
  gate when no Qt iOS kit is present to run the full build.

- [ ] **Step 1: Write the failing smoke test**

Create `tests/smoke/check_ios_gpu_build_config.sh` (mirror `check_ios_ffmpeg_build_config.sh`'s
`require_in_file`/`reject_in_file` structure):

```sh
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
CMAKE="$ROOT_DIR/CMakeLists.txt"

require_in_file() {
    local file="$1"; local needle="$2"
    if ! grep -Fq -- "$needle" "$file"; then
        echo "missing '$needle' in $file" >&2
        exit 1
    fi
}

# The iOS GPU pipeline wiring: the lifecycle source + UIKit link must be inside the
# IOS arm of the OLR_GPU_PIPELINE block, and the GPU edge .mm family must compile.
require_in_file "$CMAKE" 'ios/iosgpulifecycle.h ios/iosgpulifecycle.mm'
require_in_file "$CMAKE" '"-framework UIKit"'
require_in_file "$CMAKE" 'playback/gpu/iosgpupolicy.h playback/gpu/iosgpupolicy.cpp'
require_in_file "$CMAKE" 'playback/gpu/iosgpulifecyclesink.h playback/gpu/iosgpulifecyclesink.cpp'
require_in_file "$CMAKE" 'playback/gpu/gpurhicontext.h playback/gpu/gpurhicontext_apple.mm'
require_in_file "$CMAKE" 'playback/gpu/vtkeepsurfaceimporter.h playback/gpu/vtkeepsurfaceimporter_apple.mm'
require_in_file "$CMAKE" 'target_compile_definitions(OpenLiveReplay PRIVATE OLR_GPU_PIPELINE_BUILD=1)'

echo "iOS GPU build-config invariants present."
```

Register in `tests/smoke/CMakeLists.txt` (after the `ios_ffmpeg_build_config` test):

```cmake
add_test(NAME ios_gpu_build_config
    COMMAND "${OLR_SMOKE_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/check_ios_gpu_build_config.sh")
set_tests_properties(ios_gpu_build_config PROPERTIES LABELS "smoke;ci" TIMEOUT 10)
```

- [ ] **Step 2: Run the smoke test, expect FAIL-then-PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON
chmod +x tests/smoke/check_ios_gpu_build_config.sh
ctest --test-dir build/c -R ios_gpu_build_config --output-on-failure
```

Expected: PASS — the Task-1 CMake wiring is present, so every `require_in_file` matches. (To prove the
gate bites, temporarily delete the `"-framework UIKit"` line and re-run: it must FAIL; then restore.)

- [ ] **Step 3: Commit**

```sh
git add tests/smoke/check_ios_gpu_build_config.sh tests/smoke/CMakeLists.txt
git commit -m "test(ios-bringup): smoke-gate the iOS GPU build-config invariants"
```

---

## Task 6: Offscreen Metal render smoke + manual on-device checklist (honest validation)

**Precondition:** Tasks 1-5.

**Files:**
- Modify: `tests/unit/tst_iosgpulifecycle.cpp` (add the Metal offscreen render slot; same Metal path
  iOS uses)
- Create: `docs/superpowers/plans/2026-06-21-gpu-phase5-ios-manual-checklist.md` (the documented
  manual on-device validation steps — this is the honest boundary, not an automated test)

**Interfaces:**
- Consumes: `GpuRhiContext` (`create`, `importAndReadback`), `makeAppleNv12Surface` (Apple).
- Produces: a host-runnable offscreen Metal render smoke (the same `MTLCreateSystemDefaultDevice` /
  `QRhi::Metal` path iOS uses) + a documented manual checklist.

- [ ] **Step 1: Write the failing offscreen Metal render smoke (Apple-only, host-runnable)**

Add to `tests/unit/tst_iosgpulifecycle.cpp`:

```cpp
#ifdef __APPLE__
    void offscreenMetalRenderProducesPlanes();
#endif
```

```cpp
#ifdef __APPLE__
void TestIosGpuLifecycle::offscreenMetalRenderProducesPlanes() {
    // The offscreen Metal render smoke: the SAME QRhi::Metal /
    // MTLCreateSystemDefaultDevice path the iOS target uses, exercised on the macOS
    // host (CI has no iOS GPU). Allocate an IOSurface-backed NV12 surface, run the
    // offscreen import+readback, and assert valid planes. This is the automated
    // proxy for "the iOS Metal pipeline renders"; true on-device render is MANUAL.
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no Metal device on this host (CI/offscreen) - on-device render is manual");
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    const CpuPlanes planes = ctx->importAndReadback(surface, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 64);
    QCOMPARE(planes.height, 48);
    QVERIFY(planes.isValid());
}
#endif
```

Add the Apple include guard at the top: `#ifdef __APPLE__ #include "playback/gpu/appleiosurface.h" #endif`.

- [ ] **Step 2: Run the smoke, expect PASS (on a Metal host) / SKIP (CI)**

```sh
cmake --build build/c --target tst_iosgpulifecycle && \
  ctest --test-dir build/c -R tst_iosgpulifecycle --output-on-failure
```

Expected: PASS on a Metal-capable Mac (the offscreen Metal render round-trips); `QSKIP` on a headless
CI runner with no Metal device. **No new code needed** — the merged `GpuRhiContext`/`makeAppleNv12Surface`
already implement this; the test asserts the iOS-relevant Metal path works on the shared edge.

- [ ] **Step 3: Write the manual on-device validation checklist (the honest boundary)**

Create `docs/superpowers/plans/2026-06-21-gpu-phase5-ios-manual-checklist.md` documenting what CI
cannot cover and must be run by hand on a device with `OLR_GPU_PIPELINE=1`:

```md
# iOS GPU pipeline — manual on-device validation

CI gates the iOS **build compiling** + the **shared Metal unit tests on macOS**. The following are
MANUAL, run on a physical iOS device with the GPU pipeline enabled:

- [ ] Single-feed playback shows correct picture (no gray flash / stall) — the §8 picture-correctness
      criterion, on-device.
- [ ] Multi-feed multiview stays within the iOS GPU budget (no VRAM OOM); thermal state under sustained
      load does not crash the decode loop (degrades to CPU instead).
- [ ] Background the app, wait, foreground it: playback resumes; no stale-surface artifact, no crash
      (the generation-bump release/reacquire path).
- [ ] Lock/unlock and incoming-call interruption cycles behave like background/foreground.
- [ ] `gpuReadToCpuCount` stays at one readback per unique rendered bus surface (the copy detector),
      observed via the on-device telemetry.
```

- [ ] **Step 4: Commit**

```sh
python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add tests/unit/tst_iosgpulifecycle.cpp \
        docs/superpowers/plans/2026-06-21-gpu-phase5-ios-manual-checklist.md
git commit -m "test(ios-bringup): offscreen Metal render smoke + manual on-device checklist"
```

---

## Task 7: Pre-push iOS lane builds with the GPU pipeline on (final gate)

**Precondition:** Tasks 1-6.

**Files:**
- Modify: `.githooks/pre-push`

**Interfaces:**
- Consumes: the iOS GPU CMake wiring (Task 1).
- Produces: the pre-push iOS build lane configures with `-DOLR_GPU_PIPELINE=ON` so a GPU-edge
  compile regression is caught locally (still opt-in via `SKIP_IOS_BUILD`).

- [ ] **Step 1: Inspect the current iOS pre-push lane**

The pre-push iOS build (`.githooks/pre-push:386-391`) configures with `qt-cmake -G Xcode` **without**
`-DOLR_GPU_PIPELINE`, so it does not exercise the iOS GPU edge. Confirm that is the case.

- [ ] **Step 2: Add the GPU-on flag to the pre-push iOS configure (the change)**

Edit `.githooks/pre-push` to pass `-DOLR_GPU_PIPELINE=ON` on the iOS configure (keep the lane opt-in;
`SKIP_IOS_BUILD=1` still skips it, and it still auto-skips when the Qt iOS kit is absent):

```sh
"$QT_CMAKE" -S . -B "$BUILD_DIR" -G Xcode \
    -DQT_HOST_PATH="$QT_HOST_PREFIX" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DOLR_GPU_PIPELINE=ON >/dev/null
```

> Keep the `GIT_CONFIG_*` unset gotcha in mind: the hook runs `qt-cmake` in the push environment; if a
> harness invokes the hook with `GIT_CONFIG_*` set, the Xcode build breaks (`SKIP_IOS_BUILD=1` is the
> documented escape). This change does not alter that; it only adds the GPU flag.

- [ ] **Step 3: Run the iOS pre-push lane locally, expect PASS**

```sh
env -u GIT_CONFIG -u GIT_CONFIG_GLOBAL -u GIT_CONFIG_SYSTEM -u GIT_CONFIG_COUNT \
  OLR_PREPUSH_FULL=1 SKIP_E2E=1 SKIP_ASAN=1 SKIP_TSAN=1 SKIP_UNIT=1 SKIP_TIDY=1 \
  .githooks/pre-push origin <remote-url> < /dev/null
```

Expected: the iOS lane configures with the GPU pipeline on and **builds**. (If no Qt iOS kit, the lane
auto-skips — that is the documented fallback; the Task-5 smoke is the standing CMake-invariant gate.)

- [ ] **Step 4: Commit**

```sh
git add .githooks/pre-push
git commit -m "ci(ios-bringup): build the pre-push iOS lane with the GPU pipeline enabled"
```

---

## Done criteria

- The iOS Xcode target **compiles** with `-DOLR_GPU_PIPELINE=ON` (Task 1 + Task 7 pre-push lane;
  Task 5 smoke gates the wiring when no iOS kit is present).
- `gpuPerTrackWindowCap` / `gpuIosAggregateWindowCeiling` give iOS a **documented, tighter** GPU
  budget than macOS, wired into `gpu-budget` cap sizing (Task 2).
- Background/foreground **release+reacquire** bumps `GpuGenerationCounter` and invalidates surfaces via
  the device-loss seam, all UIKit confined to `ios/iosgpulifecycle.mm` on the main thread (Task 3).
- The render thread **never calls UIKit**; the single sanctioned present chokepoint
  (`presentOnMainThread`) marshals to the main thread on iOS (Task 4).
- The shared **Metal unit tests pass on the macOS host** and the offscreen Metal render smoke runs the
  iOS Metal path (Task 6); on-device picture/thermal/suspend correctness is **MANUAL** and documented.
- Zero-regression: with `OLR_GPU_PIPELINE` off (or the runtime gate off) the iOS app is byte-for-byte
  today's CPU-path app; the host unit suite is unchanged.
- Concurrency review obtained for Tasks 3 and 4 before merge (CLAUDE.md).

## Consumes / Produces

**Consumes:**
- Merged Apple GPU edge: `playback/gpu/{gpusurface.h,gpurhicontext.h,gpuframedata.h,
  decodedonefence.h,vtkeepsurfaceimporter.h,gpupipelineconfig.h,appleiosurface.h}` and their
  `_apple.mm` bodies (already `#ifdef __APPLE__`, compile for iOS arm64).
- `gpu-sync` keystone: `GpuGenerationCounter` (`playback/gpu/gpugeneration.h`), `GpuFence`, the
  extended `DecodeDoneFence`, the `GpuSurface` surface-lifetime hooks, the eviction lock rule.
- `gpu-compositor` (RHI compositor), `async-readback` (`AsyncGpuReadbackSink`), `gpu-budget`
  (GPU-window cap-sizing seam), `device-loss` (surface invalidation / degrade-to-CPU).
- Existing iOS scaffolding: `ios/ios_scene.{h,mm}`, `ios/Info.plist`, the `if(IOS)` CMake block, the
  `tests/smoke/check_ios_ffmpeg_build_config.sh` invariant-smoke pattern, the pre-push `SKIP_IOS_BUILD`
  lane and the `GIT_CONFIG_*`-unset gotcha.

**Produces:**
- `playback/gpu/iosgpupolicy.{h,cpp}` — platform-neutral iOS GPU budget/threading policy
  (`gpuIsIosBuild`, `gpuPerTrackWindowCap`, `gpuIosAggregateWindowCeiling`, `kIosMaxPerTrackGpuFrames`,
  `kIosAggregateGpuFrameCeiling`).
- `playback/gpu/iosgpulifecyclesink.{h,cpp}` — platform-neutral suspend/resume sink
  (`IosGpuLifecycleSink`, `DefaultIosGpuLifecycleSink`, the sink registry) bumping the GPU generation.
- `ios/iosgpulifecycle.{h,mm}` — UIKit-confined background/foreground registrar
  (`installIosGpuLifecycle`).
- `GpuRhiContext::presentOnMainThread` — the single sanctioned UIKit/main-thread present chokepoint +
  the render-thread "never call UIKit" invariant.
- iOS GPU CMake wiring (the `if(IOS)` GPU-pipeline arm); `tests/smoke/check_ios_gpu_build_config.sh`;
  `tst_iosgpupolicy`, `tst_iosgpulifecycle` (incl. the offscreen Metal render smoke);
  `docs/superpowers/plans/2026-06-21-gpu-phase5-ios-manual-checklist.md`; the pre-push iOS lane built
  with the GPU pipeline on.
