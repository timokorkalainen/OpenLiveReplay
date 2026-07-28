# Complete GPU Lifetime Enforcement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make raw and typed GPU backing escape impossible for ordinary production callers and make fenced retirement automatic after successful submission.

**Architecture:** Replace movable handle-returning leases with noncopyable callback capabilities consumed by backend-local adapters. Add a zero-allocation common-case `GpuOpScope` and a stateless `GpuRetireRegistry` facade over existing retirement storage; migrate all six manual signal/retain sites without changing GPU command order.

**Tech Stack:** C++17 templates, Qt containers/synchronization, D3D11, QRhi/Metal/IOSurface, CMake compile-negative tests, Qt Test.

## Global Constraints

- Ordinary production callers never receive a raw pointer, COM interface, IOSurface, CVPixelBuffer, device, texture, or subresource borrowed from a `GpuSurface`.
- Synchronous callbacks auto-complete on return and use no `std::function`.
- `GpuOpScope` uses one real non-null fence, signals once, and allocates nothing for one to four unique surfaces.
- No GPU wait, extra fence signal, global submission lock, duplicate retirement storage, or unnecessary reference-count churn on the normal path.
- A repeated median performance regression above 2% blocks acceptance unless independently shown to be measurement noise.
- Stage exact paths only and never bypass hooks.

---

### Task 1: Lock compile-negative protocol tests

**Files:**
- Create: `tests/gpu/negcompile/native_handle_access.cpp`
- Create: `tests/gpu/negcompile/d3d11_typed_access.cpp`
- Create: `tests/gpu/negcompile/lease_escape.cpp`
- Create: `tests/gpu/negcompile/token_mint.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: current public `GpuReadLease`, D3D typed accessors, and public mint declarations.
- Produces: CTest configure-time compile-failure gates for every bypass route.

- [ ] **Step 1: Add forbidden-use translation units**

Each source contains one use that must not compile. Examples:

```cpp
void forbidden(const GpuSurface& s) { (void)s.nativeHandle(); }
```

```cpp
void forbidden(const D3D11GpuSurface& s) {
    (void)s.texture(); (void)s.device(); (void)s.subresource();
}
```

```cpp
static_assert(!std::is_copy_constructible_v<GpuReadLease>);
static_assert(!std::is_move_constructible_v<GpuReadLease>);
void* forbidden(const GpuReadLease& lease) { return lease.nativeHandle(); }
```

```cpp
auto forbidden() { return mintDeadDeviceTokenFromFrameOp(1); }
```

- [ ] **Step 2: Register portable negative compilation**

Add a CMake helper that invokes `try_compile`, passes repository include paths and C++17, and calls `message(FATAL_ERROR ...)` if a forbidden source compiles.

Expected: configure fails before implementation because at least typed access, lease move/raw extraction, and mint lookup still compile.

- [ ] **Step 3: Commit the red protocol gate**

```powershell
git add tests/CMakeLists.txt tests/gpu/negcompile/native_handle_access.cpp tests/gpu/negcompile/d3d11_typed_access.cpp tests/gpu/negcompile/lease_escape.cpp tests/gpu/negcompile/token_mint.cpp
git commit -m "test(gpu): lock lifetime capability boundaries" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Seal the synchronous lease and backend mint authority

**Files:**
- Modify: `playback/gpu/gpusurfacelease.h`
- Modify: `playback/gpu/gpudevicelossmonitor.h`
- Modify: `playback/gpu/gpudevicelossmonitor.cpp`
- Modify: `playback/gpu/gpurhicontext_win.cpp`
- Modify: `playback/gpu/gpurhicontext_apple.mm`
- Test: `tests/unit/tst_gpusurfacelease.cpp`
- Test: `tests/unit/tst_gpu_devicelost_worker.cpp`

**Interfaces:**
- Produces: noncopyable/nonmovable `GpuReadLease`; `GpuSyncReadScope::read(surface, callback)`; atomic `publishRealDeviceLoss(provenance, observedGeneration)` owned by backend-local authority.
- Consumes: `GpuSurface::nativeHandle()` friendship and monitor `m_epochMutex`.

- [ ] **Step 1: Add callback and epoch race tests**

Test that `read()` invokes exactly once for null and non-null surfaces, returns the callback result, and has no manual `complete()`. Add concurrent clear/publish coverage that rejects a token whose observed generation differs from the active loss epoch.

- [ ] **Step 2: Replace lease return with callback execution**

Implement a forwarding template shaped as:

```cpp
template <typename Surface, typename Fn>
decltype(auto) read(Surface&& surface, Fn&& fn) {
    GpuReadLease lease(makeSurfaceOwner(std::forward<Surface>(surface)));
    return std::forward<Fn>(fn)(static_cast<const GpuReadLease&>(lease));
}
```

Delete lease copy and move construction/assignment, remove public raw extraction, and remove `complete()` state. Keep metadata inspection public.

- [ ] **Step 3: Move token construction behind backend-local authority**

Remove namespace-visible mint declarations. Backend detection functions construct provenance only after validating the authoritative failure, then publish the token and epoch together while holding `m_epochMutex`.

- [ ] **Step 4: Run focused tests and compile-negative configure**

```powershell
cmake --build build/gpu --target tst_gpusurfacelease tst_gpu_devicelost_worker
ctest --test-dir build/gpu --output-on-failure -R '^(tst_gpusurfacelease|tst_gpu_devicelost_worker)$'
```

Expected: focused tests pass and forbidden-use sources fail compilation as intended.

- [ ] **Step 5: Commit the sealed protocol**

Stage only the files in this task and commit with message `refactor(gpu): seal lease and loss authority` plus the required co-author trailer.

### Task 3: Add privileged platform operation adapters and gate typed accessors

**Files:**
- Create: `playback/gpu/applegpusurfaceops_apple.h`
- Create: `playback/gpu/applegpusurfaceops_apple.mm`
- Create: `playback/output/win/d3d11gpusurfaceops.h`
- Create: `playback/output/win/d3d11gpusurfaceops.cpp`
- Modify: `playback/output/win/d3d11gpusurface.h`
- Modify: `playback/gpu/applegpusurface_apple.mm`
- Modify: every production site currently calling `lease.nativeHandle()`, `texture()`, `device()`, or `subresource()` as enumerated by `rg`.

**Interfaces:**
- Consumes: opaque `const GpuReadLease&` only inside adapter functions.
- Produces: operation-specific functions returning `CpuPlanes`, success/status, or independently retained wrappers; never borrowed backing.

- [ ] **Step 1: Add adapter behavior tests**

Extend platform tests to prove upload/download/import/encode operations work through callback adapters and that a returned wrapper remains valid independently after callback return.

- [ ] **Step 2: Privatize all typed surface accessors**

Move D3D `texture()`, `device()`, and `subresource()` plus Apple backing extraction into private surface implementation state. Grant friendship only to named adapter entry points or private adapter implementation types.

- [ ] **Step 3: Migrate each native operation**

Use this call shape at each leaf:

```cpp
return scope.read(surface, [&](const GpuReadLease& lease) {
    return d3d11GpuSurfaceOps::copyToReadback(lease, destination);
});
```

Adapters retain/wrap backing before returning when work outlives the callback. No adapter returns a native pointer/reference.

- [ ] **Step 4: Verify platform targets**

Build all Windows GPU targets locally; rely on the macOS CI compiler/runtime gate for Objective-C++ while manually checking unlock/release placement.

- [ ] **Step 5: Commit adapter confinement**

Stage the new adapters, surface implementations, migrated call sites, and their tests; commit `refactor(gpu): confine native backing to adapters` with the required trailer.

### Task 4: Implement the retirement facade without lock-held waits

**Files:**
- Create: `playback/gpu/gpuretireregistry.h`
- Create: `playback/gpu/gpuretireregistry.cpp`
- Modify: `playback/gpu/gpureadbackretainer.h`
- Modify: `playback/gpu/gpureadbackretainer.cpp`
- Test: `tests/unit/tst_gpusurfacelease.cpp`
- Test: `tests/unit/tst_gpu_sync_stress.cpp`

**Interfaces:**
- Produces: `registerRetire(surface, fence, value)`, `drainCompleted()`, `drainWithBoundedWait(ms)`, `abandonAllNoWait(token)`, `pendingRetainCount()`, and diagnostics.
- Consumes: existing global retainer storage; no second ownership container.

- [ ] **Step 1: Add deterministic registry tests**

Cover completed collection, timeout preservation/reconciliation, high-water count, and a fake fence whose `wait()` concurrently calls `pendingRetainCount()` to prove the registry mutex is not held across waits.

- [ ] **Step 2: Implement snapshot/wait/reconcile**

Under the mutex, move or copy eligible entry identities into a local snapshot; release the mutex; call external fence waits; reacquire the mutex and erase only entries still matching the snapshot identity/value.

- [ ] **Step 3: Add low-cost telemetry**

Track pending high-water, timeout count, spill allocation count, and mutex hold duration without logging or heap allocation on successful common-path registration.

- [ ] **Step 4: Run registry and TSan-focused tests**

Expected: deterministic tests pass; TSan reports no race or lock inversion.

- [ ] **Step 5: Commit the facade**

Commit `feat(gpu): add observable retirement registry` with only registry, retainer, build, and test paths staged.

### Task 5: Implement and migrate `GpuOpScope`

**Files:**
- Create: `playback/gpu/gpuopscope.h`
- Create: `playback/gpu/gpuopscope.cpp`
- Modify: `playback/gpu/gpusurfaceallocator.cpp`
- Modify: `playback/gpu/gpuframedata.cpp`
- Modify: `playback/gpu/gpucompositor.cpp`
- Modify: `playback/output/win/wingpuimportedge.cpp`
- Modify: `playback/gpu/vtkeepsurfaceimporter_apple.mm`
- Test: `tests/unit/tst_gpusurfacelease.cpp`

**Interfaces:**
- Produces: `GpuOpScope(std::shared_ptr<GpuFence>, GpuRetireRegistry&)`, callback registration for surfaces, `submitted()`, and pre-submit `cancel()`.
- Consumes: a real fence and registry facade.

- [ ] **Step 1: Add state-machine tests**

Prove one signal for one-to-four unique surfaces, duplicate-surface coalescing, no signal/register on pre-submit cancel, safe retention plus device-health reporting on post-submit failure, and explicit failure for zero-valued signal.

- [ ] **Step 2: Implement inline common storage**

Store four `std::shared_ptr<GpuSurface>` slots inline, deduplicate by pointer, and spill only on the fifth unique surface while incrementing allocation telemetry.

- [ ] **Step 3: Implement submission finalization**

After backend submission succeeds, call `signal()` exactly once; register every unique surface with that value. Make cancellation unavailable after the submitted transition.

- [ ] **Step 4: Replace all six hand-written signal/retain sequences**

Migrate allocator mint, Apple readback, compositor output mint, Windows import mint, Windows readback retain, and VideoToolbox keep-surface import. Preserve existing submission and signal ordering.

- [ ] **Step 5: Run unit/GPU/stress tests and commit**

Commit `refactor(gpu): enforce fenced surface retirement` with the scope, six sites, tests, and build registration staged.

### Task 6: Prove performance and publish the enforcement PR

**Files:**
- Create: `tests/performance/gpu_lifetime_submission_benchmark.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml` only to add non-destructive compile/test coverage; hardware measurements remain explicitly provisioned.

**Interfaces:**
- Produces: machine-readable medians for CPU submission time, allocations, signals, mutex hold time, pending high-water, throughput, and latency.

- [ ] **Step 1: Build a repeatable parent-vs-HEAD harness**

Run identical warmup and measured iterations against the parent commit and HEAD, record at least seven samples per metric, discard warmups, and compare medians.

- [ ] **Step 2: Enforce invariants and the 2% threshold**

Fail immediately for any new GPU wait, extra signal, common-case allocation, or duplicate retirement entry. Fail when a repeated median regresses by more than 2% unless an independent rerun demonstrates noise.

- [ ] **Step 3: Run the complete repository matrix**

Run unit, GPU, e2e, ASan/UBSan, TSan GPU subset, lint, format, roadmap audit, Windows platform build, and macOS CI.

- [ ] **Step 4: Obtain independent reviews**

Require a fresh-context concurrency/security review and a separate performance-evidence review. Resolve every high-severity finding with a focused test.

- [ ] **Step 5: Push and open the next PR without merging**

Push through the credential-helper hook, open the dependency-ordered PR with measured results and adapter/scope invariants, then stop for user merge.

