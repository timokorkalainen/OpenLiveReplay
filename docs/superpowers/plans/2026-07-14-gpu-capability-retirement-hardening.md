# GPU Capability and Retirement Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the locked synchronous lease API while making asynchronous GPU submission, retirement provenance, and device-loss abandonment structurally safe and performance-bounded.

**Architecture:** Keep `GpuSurface::nativeHandle()` protected and reachable only through a public `GpuSyncReadScope`/`GpuReadLease`. Replace the separable asynchronous `track()`/`submit()` convention with typed backend adapters that prepare retirement, validate the exact fence/device/timeline, execute the native operation, signal once, and durably publish or quarantine ownership; bind dead-domain abandonment to live monitor authority under a process-wide recovery leader.

**Tech Stack:** C++17, Qt 6 Core/Gui/Test, D3D11/DXGI, Metal/IOSurface, QRhi, CMake `try_compile`, CTest, Windows manual TDR lane, sanitizer and microbenchmark gates.

## Global Constraints

- `GpuSurface::nativeHandle()` remains protected and `GpuReadLease` remains its only friend.
- `retainUntilFenceRetired()` and `pendingFenceValue()` remain public.
- All native backing acquisition remains through `GpuSyncReadScope`.
- The public synchronous pattern `scope.read(surface) -> lease.nativeHandle() -> scope.complete()` remains supported.
- Production asynchronous paths expose no public `track()` followed by an independent submission.
- No allocation, container growth, or throwing operation may occur after the driver may have accepted work.
- Retirement records carry exact fence instance, device domain, GPU generation, signal value, and owner; global surface watermarks are never paired with an arbitrary fence.
- Dead-device abandonment is authorized only while `GpuDeviceLossMonitor` keeps the matching authority epoch/loss generation stable.
- The common one-to-four-surface path is allocation-free; median and p95 steady-state overhead must each remain within 2% of the applicable #173/#174 baselines.
- The real Windows TDR lane stays manual, dedicated-runner-only, policy-gated, Job-contained, and `RelWithDebInfo`; sanitizer coverage uses the same recovery state machine non-destructively.
- Do not touch the pre-existing `expectedDecodeSurfaceBytesForTrack` warning or commit a `(void)` workaround.
- Use targeted `git add <paths>`; never stage `handoff-notes`; include `Co-Authored-By: Claude <noreply@anthropic.com>` in every commit.

---

## File Structure

- Modify `playback/gpu/gpusurfacelease.h`: explicit completion state and callback convenience while preserving public `read()`.
- Create `playback/gpu/gpusubmission.h`: typed surfaces, backend views, outcomes, prepared records, and retirement tickets.
- Modify `playback/gpu/gpuopscope.h` and `playback/gpu/gpuopscope.cpp`: fused, `noexcept` asynchronous submission with no public `track()`.
- Modify `playback/gpu/gpufence.h` plus platform fence implementations: stable fence/timeline/domain identity and typed compatibility.
- Modify `playback/gpu/gpuretireregistry.h`, `playback/gpu/gpuretireregistry.cpp`, `playback/gpu/gpureadbackretainer.h`, and `playback/gpu/gpureadbackretainer.cpp`: internal prepared publication, quarantine, and sharded pooled storage.
- Modify `playback/gpu/gpudevicelossmonitor.h` and `playback/gpu/gpudevicelossmonitor.cpp`: authority-epoch validation callback and process-wide recovery leadership.
- Modify `playback/playbackworker.h` and `playback/playbackworker.cpp`: one leader coordinates abandonment/rebuild; followers join.
- Modify D3D11, Metal, QRhi, compositor, import/readback, and encoder call sites to use fused adapters or completed synchronous scopes.
- Expand `tests/gpu/negcompile`, `tests/unit`, `tests/gpu_fault`, `tests/e2e/run_gpu_fault_recovery.ps1`, CMake, workflows, and documentation.

### Task 1: Make synchronous scope completion observable without changing the locked API

**Files:**
- Modify: `playback/gpu/gpusurfacelease.h`
- Modify: `tests/unit/tst_gpusurfacelease.cpp`
- Add: `tests/gpu/negcompile/native_handle_access_pass.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: public `GpuSyncReadScope::read()`, `complete()`, and `withRead()`; checked lease/scope state.
- Consumes: protected `GpuSurface::nativeHandle()` and existing native-owner retention.

- [ ] **Step 1: Add failing pass/fail and lifecycle tests**

Add a compile-pass control containing exactly:

```cpp
GpuSyncReadScope scope;
const GpuReadLease lease = scope.read(surface);
void* handle = lease.nativeHandle();
scope.complete();
return handle != nullptr;
```

Keep the direct `surface->nativeHandle()` compile-fail probe. Add runtime tests for completed access, `withRead()` auto-completion, handle query after completion in a death/checked-contract test, and owner survival until lease destruction.

- [ ] **Step 2: Configure/build and prove completion APIs are missing**

Run: `cmake -S . -B build/gpu-hardening -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/mingw_64 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DOLR_GPU_PIPELINE=ON -DOLR_WERROR=OFF -DOLR_BUILD_TESTS=ON -DOLR_FFMPEG_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/ffmpeg -DOLR_SRT_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/srt`

Run: `cmake --build build/gpu-hardening --target tst_gpusurfacelease`

Expected: compile failure on missing `complete()`/`withRead()` or failing lifecycle assertions.

- [ ] **Step 3: Implement shared active/completed state**

Bind each lease to a scope-owned state:

```cpp
struct GpuSyncReadState {
    bool active = true;
    bool read = false;
};

class GpuSyncReadScope final {
public:
    GpuReadLease read(const std::shared_ptr<GpuSurface>& surface);
    GpuReadLease read(GpuSurface* surface);
    void complete() noexcept;

    template <typename Surface, typename Fn>
    decltype(auto) withRead(const std::shared_ptr<Surface>& surface, Fn&& fn) {
        const GpuReadLease lease = read(surface);
        struct CompleteOnExit {
            GpuSyncReadScope* scope;
            ~CompleteOnExit() { scope->complete(); }
        } completeOnExit{this};
        return std::invoke(std::forward<Fn>(fn), lease);
    }
};
```

`GpuReadLease::nativeHandle()` asserts the state is active in checked builds. Scope destruction asserts `complete()` was called after any read; release builds emit a rate-limited diagnostic while ownership remains in the lease until destruction. Do not make the lease copyable/movable and do not hide public `read()`.

- [ ] **Step 4: Run exact compile and lifecycle controls on GCC 13.10**

Run: `cmake --build build/gpu-hardening --target tst_gpusurfacelease && ctest --test-dir build/gpu-hardening -R 'gpu_(native_handle_access|native_handle_access_pass)|tst_gpusurfacelease' --output-on-failure`

Expected: direct access fails for the intended protected-access diagnostic, pass-control compiles, runtime lifecycle tests pass, and DeadDeviceToken static assertions remain.

- [ ] **Step 5: Commit synchronous contract enforcement**

```powershell
git add playback/gpu/gpusurfacelease.h tests/unit/tst_gpusurfacelease.cpp tests/gpu/negcompile/native_handle_access_pass.cpp tests/CMakeLists.txt
git commit -m "fix(gpu): enforce synchronous lease completion" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Add exact fence/timeline/domain retirement evidence

**Files:**
- Create: `playback/gpu/gpusubmission.h`
- Modify: `playback/gpu/gpufence.h`
- Modify: `playback/gpu/gpufence_win.cpp`
- Modify: `playback/gpu/gpufence_apple.mm`
- Modify: `playback/gpu/gpufence_stub.cpp`
- Modify: `playback/gpu/gpusurface.h`
- Modify: `tests/unit/tst_gpusurfacelease.cpp`
- Modify: `tests/unit/tst_gpufence.cpp`

**Interfaces:**
- Produces: `GpuFenceIdentity`, `GpuRetirementTicket`, `GpuSurfaceCompatibility`, and exact fence validation.
- Consumes: platform device/queue identity and `GpuGenerationCounter`.

- [ ] **Step 1: Add failing wrong-fence/timeline/generation tests**

Create two fences on distinct fake domains and two fence instances on one domain. Prove a surface prepared for fence A cannot submit/retire against fence B, even when numeric signal values match. Reject stale GPU generation and reused domain pointer with a new authority epoch.

- [ ] **Step 2: Run focused fence/lease tests**

Run: `cmake --build build/gpu-hardening --target tst_gpufence tst_gpusurfacelease && ctest --test-dir build/gpu-hardening -R 'tst_(gpufence|gpusurfacelease)' --output-on-failure`

Expected: current domain-only/global-watermark behavior accepts at least one wrong pairing.

- [ ] **Step 3: Define exact evidence types**

```cpp
struct GpuFenceIdentity {
    uint64_t instanceId = 0;
    uintptr_t deviceDomainId = 0;
    uint64_t authorityEpoch = 0;
    friend bool operator==(const GpuFenceIdentity& a, const GpuFenceIdentity& b) {
        return a.instanceId == b.instanceId && a.deviceDomainId == b.deviceDomainId &&
               a.authorityEpoch == b.authorityEpoch;
    }
};

struct GpuRetirementTicket {
    std::shared_ptr<GpuFence> fence;
    GpuFenceIdentity identity;
    uint64_t gpuGeneration = 0;
    uint64_t value = 0;
};

struct GpuSurfaceCompatibility {
    uintptr_t deviceDomainId = 0;
    uint64_t authorityEpoch = 0;
};
```

Add `GpuFence::identity()` and `GpuSurface::compatibility()` without exposing native handles. Every fence receives a process-wide monotonic instance ID that is never derived from a reusable object address. D3D11 domain identity uses the retained `ID3D11Device`; Metal uses retained `MTLDevice`/command queue identity plus authority epoch. Compatibility must be checked before any native call can submit.

- [ ] **Step 4: Run wrong-evidence tests**

Run: `cmake --build build/gpu-hardening --target tst_gpufence tst_gpusurfacelease && ctest --test-dir build/gpu-hardening -R 'tst_(gpufence|gpusurfacelease)' --output-on-failure`

Expected: wrong instance/domain/generation/authority tests reject before submission.

- [ ] **Step 5: Commit exact provenance**

```powershell
git add playback/gpu/gpusubmission.h playback/gpu/gpufence.h playback/gpu/gpufence_win.cpp playback/gpu/gpufence_apple.mm playback/gpu/gpufence_stub.cpp playback/gpu/gpusurface.h tests/unit/tst_gpusurfacelease.cpp tests/unit/tst_gpufence.cpp
git commit -m "feat(gpu): bind retirement to exact fence timelines" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: Fuse asynchronous submission with prepared retirement and quarantine

**Files:**
- Modify: `playback/gpu/gpusubmission.h`
- Modify: `playback/gpu/gpuopscope.h`
- Modify: `playback/gpu/gpuopscope.cpp`
- Modify: `playback/gpu/gpuretireregistry.h`
- Modify: `playback/gpu/gpuretireregistry.cpp`
- Modify: `tests/unit/tst_gpusurfacelease.cpp`
- Add: `tests/gpu/negcompile/gpu_op_track_access.cpp`
- Add: `tests/gpu/negcompile/gpu_submit_without_owner.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: fused `GpuOpScope::submit(BackendCommand, SurfacePack)` and internal prepared/quarantine publication.
- Consumes: exact Task 2 compatibility and retirement evidence.

- [ ] **Step 1: Add failing compile/runtime tests**

Require arbitrary code calling `operation.track(surface)` to fail compilation. Add pass controls for typed D3D11/Metal/stub adapters. Runtime tests cover incompatible fence rejection before callback, one-to-four surfaces with zero allocation, overflow reservation before callback, `NotSubmitted`, `Submitted`, `SubmittedWithError`, zero signal quarantine, and an injected allocation failure before/after the possible-submit boundary.

- [ ] **Step 2: Run tests and expose the separable API**

Run: `cmake --build build/gpu-hardening --target tst_gpusurfacelease && ctest --test-dir build/gpu-hardening -R 'gpu_(op_track_access|submit_without_owner)|tst_gpusurfacelease' --output-on-failure`

Expected: compile-fail probes unexpectedly compile or runtime ownership mutations fail.

- [ ] **Step 3: Replace public `track()` with fused typed submission**

Expose a surface pack that owns every surface before invoking the backend:

```cpp
enum class GpuSubmitOutcome : uint8_t { NotSubmitted, Submitted, SubmittedWithError };

template <size_t N>
class GpuSurfacePack final {
public:
    explicit GpuSurfacePack(std::array<std::shared_ptr<GpuSurface>, N> surfaces);
    const std::array<std::shared_ptr<GpuSurface>, N>& owners() const noexcept;
};

class GpuOpScope final {
public:
    template <typename BackendAdapter, size_t N>
    GpuSubmissionResult submit(BackendAdapter& adapter,
                               GpuSurfacePack<N> surfaces) noexcept;
};
```

Before adapter invocation: validate every owner/fence, reserve a prepared retirement node for each owner, and acquire leases inside the privileged adapter. After the adapter may submit: perform only `noexcept` signal/publication moves into the signaled queue or preallocated device quarantine. Destructor of an unsubmitted scope returns prepared nodes and owners; destructor after possible submission must never drop them.

- [ ] **Step 4: Run fused-submission and negative-compile tests**

Run: `cmake --build build/gpu-hardening --target tst_gpusurfacelease && ctest --test-dir build/gpu-hardening -R 'gpu_(op_track_access|submit_without_owner)|tst_gpusurfacelease' --output-on-failure`

Expected: both forbidden APIs fail for their intended reason; all ownership/failure outcomes pass.

- [ ] **Step 5: Commit fused submission**

```powershell
git add playback/gpu/gpusubmission.h playback/gpu/gpuopscope.h playback/gpu/gpuopscope.cpp playback/gpu/gpuretireregistry.h playback/gpu/gpuretireregistry.cpp tests/unit/tst_gpusurfacelease.cpp tests/gpu/negcompile/gpu_op_track_access.cpp tests/gpu/negcompile/gpu_submit_without_owner.cpp tests/CMakeLists.txt
git commit -m "fix(gpu): fuse submission with durable retirement" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Replace the global copy/scan registry with sharded pooled queues

**Files:**
- Modify: `playback/gpu/gpuretireregistry.h`
- Modify: `playback/gpu/gpuretireregistry.cpp`
- Modify: `playback/gpu/gpureadbackretainer.h`
- Modify: `playback/gpu/gpureadbackretainer.cpp`
- Modify: `tests/unit/tst_gpusurfacelease.cpp`
- Create: `tests/perf/tst_gpuretireregistry_perf.cpp`
- Create: `tests/perf/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: pooled prepared nodes, shard-local publication/drain/abandon, atomic diagnostics.
- Consumes: `GpuFenceIdentity` and Task 3 prepared records.

- [ ] **Step 1: Add concurrency/allocation/performance tests**

Run 8 producer threads across 16 exact fence identities while one drainer advances watermarks. Assert no lost/double release, one completion query per distinct fence per drain, no common-path allocation after warm-up, and abandonment visits only matching domain shards.

- [ ] **Step 2: Establish the current performance/allocation baseline**

Run: `cmake --build build/gpu-hardening --target tst_gpuretireregistry_perf && ctest --test-dir build/gpu-hardening -R '^tst_gpuretireregistry_perf$' --output-on-failure`

Expected before implementation: allocation or global-lock/copy-scan assertions fail.

- [ ] **Step 3: Implement fixed-capacity pooled shards**

Use a power-of-two shard array keyed by `GpuFenceIdentity`. A prepared node contains ticket, owner, and intrusive next pointer. Allocate blocks only in `prepare()` before submission; publish/unlink under the shard mutex; query each distinct fence outside locks; atomically maintain pending/high-water/timeout/signal-failure counters. Remove `QVector` full copies and `QSet` scans from the common path.

- [ ] **Step 4: Run stress and performance tests**

Run: `cmake --build build/gpu-hardening --target tst_gpusurfacelease tst_gpuretireregistry_perf && ctest --test-dir build/gpu-hardening -R 'tst_(gpusurfacelease|gpuretireregistry_perf)' --output-on-failure`

Expected: correctness/stress pass and warmed common path reports zero allocations.

- [ ] **Step 5: Commit sharded retirement storage**

```powershell
git add playback/gpu/gpuretireregistry.h playback/gpu/gpuretireregistry.cpp playback/gpu/gpureadbackretainer.h playback/gpu/gpureadbackretainer.cpp tests/unit/tst_gpusurfacelease.cpp tests/perf/tst_gpuretireregistry_perf.cpp tests/perf/CMakeLists.txt tests/CMakeLists.txt
git commit -m "perf(gpu): shard and pool retirement queues" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5: Bind abandonment to live authority and serialize multi-worker recovery

**Files:**
- Modify: `playback/gpu/gpusurfacelease.h`
- Modify: `playback/gpu/gpudevicelossmonitor.h`
- Modify: `playback/gpu/gpudevicelossmonitor.cpp`
- Create: `playback/gpu/gpurecoverycoordinator.h`
- Create: `playback/gpu/gpurecoverycoordinator.cpp`
- Modify: `playback/playbackworker.h`
- Modify: `playback/playbackworker.cpp`
- Modify: `tests/unit/tst_devicelossmonitor.cpp`
- Modify: `tests/unit/tst_gpu_devicelost_worker.cpp`

**Interfaces:**
- Produces: `withValidatedDeadDomains(fn)` and one process-wide recovery leader per loss generation.
- Consumes: registry domain-abandon callback and exact authority epoch/generation.

- [ ] **Step 1: Add failing stale-token and multi-worker tests**

Copy a token, begin rebuild, register a new record under a reused domain ID, and prove the copied token cannot release it. Add two workers observing one loss simultaneously; assert one abandonment/rebuild, followers join the same result, multiple proven dead domains abandon together, and unmatched live domains receive one total bounded wait.

- [ ] **Step 2: Run monitor/worker tests**

Run: `cmake --build build/gpu-hardening --target tst_devicelossmonitor tst_gpu_devicelost_worker && ctest --test-dir build/gpu-hardening -R 'tst_(devicelossmonitor|gpu_devicelost_worker)' --output-on-failure`

Expected: copied-token and multiple-worker authority assertions fail on the current public token path.

- [ ] **Step 3: Make tokens evidence-only and validate under lock**

Remove public registry abandonment overloads accepting `DeadDeviceToken`. Add:

```cpp
enum class GpuValidatedLossStatus : uint8_t { Rejected, Completed };

struct GpuValidatedLossResult {
    GpuValidatedLossStatus status = GpuValidatedLossStatus::Rejected;
    qsizetype abandoned = 0;
};

template <typename Fn>
GpuValidatedLossResult GpuDeviceLossMonitor::withValidatedDeadDomains(Fn&& fn) {
    std::unique_lock<std::mutex> epochLock(m_epochMutex);
    const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
    auto leadership = GpuRecoveryCoordinator::instance().joinOrLead(generation);
    if (!leadership.isLeader()) return leadership.waitForResult();
    if (!m_lost.load(std::memory_order_acquire) || m_rebuildInProgress ||
        !proofsMatchCurrentEpoch())
        return leadership.finish({GpuValidatedLossStatus::Rejected, 0});
    const auto domains = validatedDomainsLocked();
    const auto result = std::invoke(std::forward<Fn>(fn), domains);
    return leadership.finish(result);
}
```

Keep lock order `m_epochMutex -> recovery leader -> registry shard`; registry code never calls the monitor while holding a shard. `beginRebuild()` invalidates authority before replacement device/queue/records register.

- [ ] **Step 4: Run authority and multi-worker tests repeatedly**

Run: `1..20 | ForEach-Object { ctest --test-dir build/gpu-hardening -R 'tst_(devicelossmonitor|gpu_devicelost_worker)' --output-on-failure; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: twenty passes, one leader per loss generation, no stale abandonment.

- [ ] **Step 5: Commit authoritative recovery**

```powershell
git add playback/gpu/gpusurfacelease.h playback/gpu/gpudevicelossmonitor.h playback/gpu/gpudevicelossmonitor.cpp playback/gpu/gpurecoverycoordinator.h playback/gpu/gpurecoverycoordinator.cpp playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_devicelossmonitor.cpp tests/unit/tst_gpu_devicelost_worker.cpp
git commit -m "fix(gpu): bind abandonment to recovery authority" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 6: Migrate production D3D11, Metal, QRhi, readback, and encoder paths

**Files:**
- Modify: `playback/gpu/gpucompositor.cpp`
- Modify: `playback/gpu/gpucompositor_apple.mm`
- Modify: `playback/gpu/gpurhicontext_win.cpp`
- Modify: `playback/gpu/gpurhicontext_apple.mm`
- Modify: `playback/gpu/gpusurfaceallocator.cpp`
- Modify: `playback/gpu/gpuframedata.cpp`
- Modify: `playback/gpu/vtkeepsurfaceimporter_apple.mm`
- Modify: `playback/gpu/applegpusurface_apple.mm`
- Modify: `playback/output/win/wingpuimportedge.cpp`
- Modify: `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp`
- Modify: `recorder_engine/codec/nativevideoencoder_videotoolbox.mm`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `tests/unit/tst_wingpuimportedge.cpp`
- Modify: `tests/unit/tst_gpurhicontext.cpp`
- Modify: `tests/unit/tst_streamworker_gpuencode.cpp`
- Modify: `tests/unit/tst_vtiosurface.cpp`

**Interfaces:**
- Produces: no production asynchronous `track()`/manual arbitrary fence pairing; every synchronous scope completes.
- Consumes: fused adapters, exact tickets, and public synchronous lease API.

- [ ] **Step 1: Build first and record exact failing/mutating sites**

Run: `cmake --build build/gpu-hardening`

Expected during migration: compiler identifies every removed `track()`/registry path and any scope lacking `complete()`.

- [ ] **Step 2: Migrate D3D11/QRhi paths and run Windows tests**

Use typed adapters that receive `D3D11GpuView`/`QrhiGpuView` inside fused submission. Keep synchronous inspection in `GpuSyncReadScope`, calling `complete()` immediately after the native API finishes. Do not change test fakes that intentionally expose their own public override.

Run: `cmake --build build/gpu-hardening --target tst_wingpuimportedge tst_gpurhicontext tst_streamworker_gpuencode && ctest --test-dir build/gpu-hardening -R 'tst_(wingpuimportedge|gpurhicontext|streamworker_gpuencode)' --output-on-failure`

Expected: all Windows focused tests pass, including wrong-device rejection before submit.

- [ ] **Step 3: Re-read and migrate all Apple paths**

For every Metal/IOSurface scope, retain the backing across the exact native call, call `complete()` after the command encoding or synchronous VideoToolbox operation, and transfer owners to the command-buffer completion retirement record only after accepted submission. Preserve CFRetain/CFRelease balance and old-device/new-queue authority identity.

- [ ] **Step 4: Add Apple platform assertions**

Extend tests to count retains/releases, prove command-buffer completion runs after submission, reject reused `MTLDevice` identity with a new queue/authority epoch, and cover VideoToolbox encoder completion placement.

- [ ] **Step 5: Run the full Windows GPU-on unit suite**

Run: `cmake --build build/gpu-hardening && ctest --test-dir build/gpu-hardening -L unit --output-on-failure`

Expected: complete local unit suite passes with `OLR_WERROR=OFF`; no source change touches `expectedDecodeSurfaceBytesForTrack`.

- [ ] **Step 6: Commit production migration**

```powershell
git add playback/gpu/gpucompositor.cpp playback/gpu/gpucompositor_apple.mm playback/gpu/gpurhicontext_win.cpp playback/gpu/gpurhicontext_apple.mm playback/gpu/gpusurfaceallocator.cpp playback/gpu/gpuframedata.cpp playback/gpu/vtkeepsurfaceimporter_apple.mm playback/gpu/applegpusurface_apple.mm playback/output/win/wingpuimportedge.cpp recorder_engine/codec/nativevideoencoder_mediafoundation.cpp recorder_engine/codec/nativevideoencoder_videotoolbox.mm recorder_engine/streamworker.cpp tests/unit/tst_wingpuimportedge.cpp tests/unit/tst_gpurhicontext.cpp tests/unit/tst_streamworker_gpuencode.cpp tests/unit/tst_vtiosurface.cpp
git commit -m "refactor(gpu): route operations through fused adapters" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 7: Exact negative compilation and source-coupled mutations

**Files:**
- Add: `tests/gpu/negcompile/gpu_submission_view_unwrap.cpp`
- Add: `tests/gpu/negcompile/gpu_registry_pairing.cpp`
- Add: `tests/gpu/negcompile/gpu_token_abandon.cpp`
- Add: `tests/gpu/negcompile/gpu_submission_pass.cpp`
- Create: `tests/gpu/gpu_capability_source_audit.py`
- Create: `tests/mutations/gpu_retirement_mutations.cmake`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `playback/gpu/gpuopscope.cpp`
- Modify: `playback/gpu/gpudevicelossmonitor.cpp`

**Interfaces:**
- Produces: exact compile pass/fail controls and mutations for compatibility, generation, durable publication, and quarantine transfer.
- Consumes: capability APIs from Tasks 1-6.

- [ ] **Step 1: Add paired compile controls**

Every failure probe includes the expected forbidden expression and CMake checks the diagnostic contains its symbol; every concept also has a pass control. Cover base/derived handle access, opaque view unwrap, submit without owners, arbitrary registry pairing, token construction, and direct abandonment.

- [ ] **Step 2: Add narrow compiled mutations**

Build variants with `OLR_MUTATE_GPU_COMPATIBILITY`, `OLR_MUTATE_GPU_LOSS_GENERATION`, `OLR_MUTATE_GPU_RETIRE_PUBLICATION`, and `OLR_MUTATE_GPU_QUARANTINE_TRANSFER`. Each CTest passes only when its named runtime test fails under the mutant.

- [ ] **Step 3: Add the production capability source audit**

Scan production translation units and require direct `GpuSurface::nativeHandle()` access to remain confined to `gpusurfacelease.h`; permit public `GpuSyncReadScope::read()` only in an explicit reviewed synchronous-adapter allowlist; require each such lexical scope to call `complete()` or use `withRead()`; reject a public `GpuRetireRegistry::registerRetire()` or `GpuOpScope::track()`. The audit prints the exact path/line and forbidden expression.

- [ ] **Step 4: Run compile, source-audit, and mutation gates**

Run: `ctest --test-dir build/gpu-hardening -R 'gpu_(negcompile|compile_pass|capability_source_audit|retirement_mutation_)' --output-on-failure`

Expected: every forbidden expression fails for its intended capability boundary, pass controls compile, and every mutation is killed.

- [ ] **Step 5: Commit falsifiability gates**

```powershell
git add tests/gpu/negcompile/gpu_submission_view_unwrap.cpp tests/gpu/negcompile/gpu_registry_pairing.cpp tests/gpu/negcompile/gpu_token_abandon.cpp tests/gpu/negcompile/gpu_submission_pass.cpp tests/gpu/gpu_capability_source_audit.py tests/mutations/gpu_retirement_mutations.cmake tests/CMakeLists.txt tests/unit/CMakeLists.txt playback/gpu/gpuopscope.cpp playback/gpu/gpudevicelossmonitor.cpp
git commit -m "test(gpu): falsify capability and retirement bypasses" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 8: Complete the Windows fault lane and non-destructive sanitizer matrix

**Files:**
- Modify: `tests/gpu_fault/win_gpu_fault_child.cpp`
- Modify: `tests/gpu_fault/win_gpu_fault_parent.cpp`
- Modify: `tests/gpu_fault/win_gpu_fault_worker_oracle.h`
- Modify: `tests/gpu_fault/win_gpu_fault_worker_oracle.cpp`
- Modify: `tests/gpu_fault/win_gpu_fault_safety.h`
- Modify: `tests/gpu_fault/CMakeLists.txt`
- Modify: `tests/e2e/run_gpu_fault_recovery.ps1`
- Modify: `.github/workflows/windows-gpu-fault.yml`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/testing/windows-gpu-fault-lane.md`

**Interfaces:**
- Produces: exact NV12 admission, truthful real-TDR evidence, and sanitizer execution of the same recovery state machine.
- Consumes: authoritative loss/recovery code from Task 5.

- [ ] **Step 1: Add failing capability/mutation assertions**

Admission must create the exact NV12 plane views, fence, shader, and worker cache resources without dispatching the destructive shader. Add mutations for authoritative upgrade removal, recovery abandonment removal, stale-frame acceptance, and worker-output non-resumption.

- [ ] **Step 2: Implement the non-destructive recovery state-machine harness**

Run injected/tokenless and fake-authoritative domains through the production coordinator under ASan/UBSan/TSan. Assert stale frames are rejected, retired owners are released only with valid authority, and output resumes with discrete post-loss submissions.

- [ ] **Step 3: Tighten real-lane evidence and claims**

Keep the destructive lane `RelWithDebInfo`, manual, policy-gated, dedicated-runner-only, and Job-contained. Record adapter/domain identity, pre/post submission sequence IDs, recovery duration, mutation results, and child process containment. Do not claim ASan on the TDR binary or continuous zero-gray output without a continuous sampler.

- [ ] **Step 4: Run non-destructive local gates**

Run: `ctest --test-dir build/gpu-hardening -R 'gpu_(recovery_state_machine|fault_mutation_)' --output-on-failure`

Expected: recovery state machine and every fault mutation pass differentially. The destructive command remains a dedicated-runner manual gate:

`pwsh -NoProfile -ExecutionPolicy Bypass -File tests/e2e/run_gpu_fault_recovery.ps1 build/gpu-fault`

- [ ] **Step 5: Commit full fault support**

```powershell
git add tests/gpu_fault/win_gpu_fault_child.cpp tests/gpu_fault/win_gpu_fault_parent.cpp tests/gpu_fault/win_gpu_fault_worker_oracle.h tests/gpu_fault/win_gpu_fault_worker_oracle.cpp tests/gpu_fault/win_gpu_fault_safety.h tests/gpu_fault/CMakeLists.txt tests/e2e/run_gpu_fault_recovery.ps1 .github/workflows/windows-gpu-fault.yml .github/workflows/ci.yml docs/testing/windows-gpu-fault-lane.md
git commit -m "test(gpu): complete authoritative fault recovery gates" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 9: Performance comparison, full verification, and documentation

**Files:**
- Modify: `tests/perf/tst_gpuretireregistry_perf.cpp`
- Create: `tests/perf/tst_gpuoperation_perf.cpp`
- Modify: `tests/perf/CMakeLists.txt`
- Modify: `docs/hardest-technical-challenges.md`
- Modify: `docs/build-and-run.md`

**Interfaces:**
- Produces: #173/#174 median+p95 comparisons and accurate final claims.
- Consumes: complete fused/sharded implementation.

- [ ] **Step 1: Benchmark compositor/import/readback steady state**

Build baseline executables from `39b94443` and `003a2bcf`, warm pools, run at least 100000 one-to-four-surface operations, and compare median and p95. Fail if either ratio is greater than 1.02. Report allocations, shard contention, completion queries, and quarantine count.

- [ ] **Step 2: Run complete Windows verification**

```powershell
cmake --build build/gpu-hardening
ctest --test-dir build/gpu-hardening -L unit --output-on-failure
ctest --test-dir build/gpu-hardening -R 'gpu_(negcompile|compile_pass|retirement_mutation_|recovery_state_machine|fault_mutation_)' --output-on-failure
ctest --test-dir build/gpu-hardening -R 'tst_gpu(operation|retireregistry)_perf' --output-on-failure
python tools/roadmap/audit.py
git diff --check
```

Expected: all gates pass, both performance ratios are at most 1.02, roadmap audit reports zero problems, and diff check is silent.

- [ ] **Step 3: Update documentation honestly**

Document the public synchronous pointer boundary, fused asynchronous guarantee, exact ticket provenance, failed-signal quarantine, recovery lock order, multi-worker leader behavior, benchmark numbers, platform coverage, and the real fault lane’s precise limitations.

- [ ] **Step 4: Commit performance and documentation evidence**

```powershell
git add tests/perf/tst_gpuretireregistry_perf.cpp tests/perf/tst_gpuoperation_perf.cpp tests/perf/CMakeLists.txt docs/hardest-technical-challenges.md docs/build-and-run.md
git commit -m "perf(gpu): gate capability retirement overhead" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 10: macOS validation, independent security review, and merge-ready handoff

**Files:**
- Modify only paths required by reproduced CI/review findings.

**Interfaces:**
- Consumes: Windows evidence, macOS CI, and complete branch.
- Produces: review-clean PR update; no automatic merge.

- [ ] **Step 1: Push through the documented hook**

```powershell
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin feat/windows-gpu-fault-lane
```

Expected: hook runs without `--no-verify`. If the known local GCC warning blocks only its documented gate, use the narrow documented skip flag and leave all other gates enabled.

- [ ] **Step 2: Require macOS CI as the Apple gate**

Confirm Apple compilation/tests exercise CFRetain/CFRelease balance, command-buffer completion placement, VideoToolbox scope completion, old-device/new-queue rejection, and multi-domain authoritative recovery.

- [ ] **Step 3: Request a fresh-context GPU concurrency/security review**

Require review of: native capability boundaries, possible-submit exception/allocation safety, exact fence compatibility, quarantine ownership, device-loss lock order, authority under `m_epochMutex`, process-wide leadership, Apple completion placement, sharded-registry contention, and performance evidence.

- [ ] **Step 4: Fix every Critical/Important finding test-first**

Reproduce each finding with a compile probe, deterministic runtime test, sanitizer test, or benchmark; apply the smallest design-consistent correction; rerun focused/full gates and macOS CI.

- [ ] **Step 5: Commit review corrections as focused changes**

Stage only the files belonging to each reproduced finding and commit them with the required co-author trailer. Confirm `git status --short` lists no uncommitted production or test changes.

- [ ] **Step 6: Update/open the PR and stop before merge**

The PR body must explain the three locked design decisions, the strengthened asynchronous architecture, exact verification, performance results, Apple CI, and fault-lane limitations. Hand the clean PR to the user; do not auto-merge.
