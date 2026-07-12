# GPU Surface Lease Foundation CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make PR #173 portable and green without weakening the protected native-handle or epoch-bound device-loss guarantees.

**Architecture:** Keep the committed lease design intact and repair only target composition: the always-built lease test must receive the readback-retainer implementation in GPU-off configurations. Validate the same source under GCC, sanitizers, Windows GPU-on, and macOS CI.

**Tech Stack:** C++17, Qt 6 Test, CMake 3.21+, Ninja, GCC 13.1/13.10, Clang sanitizers, GitHub Actions.

## Global Constraints

- Do not change the committed PR #173 design or the pre-existing `expectedDecodeSurfaceBytesForTrack` warning.
- Keep `nativeHandle()` protected and friend only `GpuReadLease`; keep retain/pending public.
- Use `-DOLR_WERROR=OFF` only for the documented local MinGW warning.
- Stage exact paths only; never use `git add -A` because `handoff-notes/` is local-only.
- Never bypass hooks with `--no-verify`.

---

### Task 1: Supply the retainer implementation to the GPU-off lease test

**Files:**
- Modify: `tests/unit/CMakeLists.txt:139`
- Test: `tests/unit/tst_gpusurfacelease.cpp`

**Interfaces:**
- Consumes: `gpuRetainSurfaceUntilFenceRetired(...)` and `gpuDrainReadbackRetainsWithBoundedWait(int)` from `playback/gpu/gpureadbackretainer.h`.
- Produces: a link-complete `tst_gpusurfacelease` target for both `OLR_GPU_PIPELINE=OFF` and `ON`.

- [ ] **Step 1: Reproduce the GPU-off link failure**

Run:

```powershell
cmake -S . -B build/gpu-off -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=OFF -DOLR_WERROR=OFF
cmake --build build/gpu-off --target tst_gpusurfacelease
```

Expected: link failure naming `gpuRetainSurfaceUntilFenceRetired` and `gpuDrainReadbackRetainsWithBoundedWait`.

- [ ] **Step 2: Add the implementation only where the aggregate test library omits it**

Insert immediately after `olr_add_unit_test(tst_gpusurfacelease olr_test_playback)`:

```cmake
if(NOT OLR_GPU_PIPELINE)
    target_sources(tst_gpusurfacelease PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpureadbackretainer.cpp"
    )
endif()
```

This avoids duplicate definitions in GPU-on builds, where `olr_test_playback` already contains the source.

- [ ] **Step 3: Verify the focused GPU-off test**

Run:

```powershell
cmake --build build/gpu-off --target tst_gpusurfacelease
ctest --test-dir build/gpu-off --output-on-failure -R '^tst_gpusurfacelease$'
```

Expected: build succeeds and one test passes.

- [ ] **Step 4: Commit the portable target fix**

```powershell
git add tests/unit/CMakeLists.txt
git commit -m "fix(test): link surface retainer in gpu-off builds" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Run the required local verification matrix

**Files:**
- Verify: `tests/unit/tst_gpusurfacelease.cpp`
- Verify: `playback/gpu/gpusurfacelease.h`
- Verify: five Apple call-site edits and two Apple/Windows mint sites listed in `handoff-notes/pr3_handoff.md`

**Interfaces:**
- Consumes: the completed PR #173 source tree.
- Produces: compiler, unit, sanitizer, and platform evidence suitable for PR review.

- [ ] **Step 1: Build and test Windows GPU-on**

Run the exact configure/build/test commands from `handoff-notes/pr3_handoff.md` section 2 with `-DOLR_WERROR=OFF`.

Expected: configure and build succeed; `ctest -L unit` reports zero failures.

- [ ] **Step 2: Verify GCC protected-access behavior**

Run the GPU-off focused build with the repository's supported GCC compiler.

Expected: `CanCallNativeHandle` remains a soft SFINAE failure and the translation unit compiles. If the compiler produces a protected-access hard error, replace only that probe with a CMake `try_compile` negative test whose source contains:

```cpp
#include "playback/gpu/gpusurface.h"
void forbidden(const GpuSurface& surface) {
    (void)surface.nativeHandle();
}
```

The CMake test passes only when compilation fails; the `DeadDeviceToken` construction assertions remain in `tst_gpusurfacelease.cpp`.

- [ ] **Step 3: Re-read platform edits for lifetime-boundary placement**

Confirm each Apple scope completes only after the native operation, unlock, wrapper retention, or render-thread invocation has returned, and confirm each mint is reachable only from a driver-authoritative failure branch.

Expected: no `complete()` before the last native use and no token mint reachable from injection.

- [ ] **Step 4: Run repository gates**

```powershell
python tools/roadmap/audit.py
git diff --check origin/main...HEAD
```

Expected: both commands exit zero.

### Task 3: Publish and close the PR #173 gate

**Files:**
- Modify only if CI exposes a source defect: files named by that failing compiler/test.

**Interfaces:**
- Consumes: locally verified commits.
- Produces: a green PR #173 and recorded independent concurrency/security approval.

- [ ] **Step 1: Push through the repository hook**

```powershell
git -c credential.helper= -c "credential.helper=!gh auth git-credential" push -u origin fix/gpu-surface-lease-lifetime
```

Expected: push succeeds. If only the documented pre-existing delivery warning gate fails, use its documented selective environment flag while retaining all other gates.

- [ ] **Step 2: Inspect every required check**

```powershell
gh pr checks 173 --watch
```

Expected: every required check succeeds. Diagnose any failure from its first causal error and add a focused failing test before changing source.

- [ ] **Step 3: Record independent review outcome**

Ensure the fresh-context GPU review covers device-loss lock order, token/epoch publication under `m_epochMutex`, and Apple completion placement.

Expected: no unresolved high-severity lifetime or concurrency finding.

