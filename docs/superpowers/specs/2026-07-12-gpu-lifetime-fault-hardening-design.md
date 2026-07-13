# GPU Lifetime and Real-Fault Hardening Design

## Goal

Make the GPU pipeline safer, easier to extend, and more recoverable without adding a material hot-path performance cost. The completed program must make unsafe native-backing access structurally unavailable, make fenced retirement unavoidable after submission, and verify recovery against a genuine Windows driver-observed device failure.

## Delivery sequence

The program is delivered as three consecutive, dependency-ordered pull requests. Splitting review units does not defer any part of the program.

1. **PR #173 — safe foundation**
   - Fix every required Linux and sanitizer CI failure introduced or exposed by the surface-lease work.
   - Retain the protected `nativeHandle()` boundary, epoch-bound `DeadDeviceToken`, corrected Apple completion placement, and compiler-driven platform test migrations.
   - Do not declare the foundation merge-ready until the required CI gate is green.
2. **PR #174 — complete lifetime enforcement**
   - Replace escapable handle leases with structurally bounded access.
   - Gate typed platform accessors as well as the base native handle.
   - Introduce fenced operation scope and retirement-registry enforcement.
   - Migrate every production GPU operation and add compile-negative coverage for every supported bypass route.
3. **PR #175 — real Windows fault verification**
   - Add opt-in, child-process-isolated real-fence and genuine device-removal tests.
   - Preserve complete driver and recovery evidence.
   - Fail closed when explicitly enabled on supported hardware.

## End-state invariant

No ordinary production caller can obtain a raw or typed GPU backing. Raw extraction is confined to small privileged backend-adapter translation units, and callers can invoke those adapters only with a scope-owned lease capability. Every operation follows one of two structurally bounded protocols:

- a synchronous access whose backing cannot escape the callback and whose operation completes before the scope returns; or
- a fenced submission that automatically signals once, stamps every participating surface, and registers every surface for retirement.

The API must not rely on a debug-only assertion or a caller-controlled `complete()` boolean as the sole enforcement mechanism.

## Synchronous handle access

Synchronous access uses a templated callback so the compiler can inline the call and the lease capability cannot be copied, moved, or returned:

```cpp
scope.read(surface, [&](const GpuReadLease& lease) {
    appleSurfaceOps::downloadToCpu(lease, destination);
});
```

`GpuReadLease` is non-copyable and non-movable. Its constructor and raw/typed backing access remain private. Ordinary callers may inspect metadata and pass the lease to a privileged backend operation, but cannot retrieve a pointer, COM interface, IOSurface, CVPixelBuffer, device, texture, or subresource from it. The API must not use `std::function` on the hot path.

The privileged adapter performs the native operation inside its translation unit and returns only copied data, status, or a resource with an independent retained lifetime. It never returns a borrowed native backing. Adding a new native GPU operation therefore requires an explicit adapter entry point rather than a generic pointer escape hatch.

Returning from the callback automatically completes the synchronous scope; there is no caller-controlled `complete()` switch. The callback return boundary must follow the operation that establishes independent lifetime or finishes the read:

- CPU download: after unlock and wrapper release;
- CPU upload: after the copy and unlock complete;
- QRhi import/readback: after the render-thread invocation returns;
- VideoToolbox encode: after submission/completion returns and the wrapper is released;
- wrapper creation: after the created wrapper has taken its own backing reference.

Exceptions and early returns must still discharge the scope. Where exceptions are disabled or absent, explicit result types carry success/failure without weakening the lifetime boundary.

## Typed platform-accessor gating

The base `nativeHandle()` gate is insufficient if callers can reach the same resource through a public platform accessor. The following routes become private to the surface and privileged backend adapters:

- D3D11 texture, device, and subresource access;
- Apple IOSurface/CVPixelBuffer backing access;
- any future platform-native pointer or integer handle that aliases surface storage.

Metadata that cannot release or dereference backing storage may remain public. Compile-negative tests must prove that base handles, each typed accessor, and raw extraction through `GpuReadLease` are inaccessible from ordinary production code.

## Fenced operation scope

`GpuOpScope` represents one submitted GPU operation. It owns a non-null real fence, references the retirement facade, and uses inline storage for the common surface count.

Before submission, callers register participating surfaces through the scoped callback API. The callback may call a privileged backend submit adapter with the opaque lease but cannot extract or retain the backing itself. Once submission succeeds, scope finalization performs exactly once:

1. signal the fence once;
2. stamp every participating surface with the returned value using monotonic maximum semantics;
3. register every surface and fence/value pair for retirement.

Cancellation is allowed only before GPU work is submitted. A submitted operation cannot opt out of retirement. A zero signal value or backend submission error is surfaced as an explicit failure and device-health event rather than silently dropping retention.

Common operations involving one to four surfaces use fixed inline storage and allocate nothing. Larger operations may spill to dynamic storage with allocation telemetry.

## Retirement registry

`GpuRetireRegistry` is the sole public interface for surface retirement, but it does not duplicate existing storage merely to create another abstraction. It provides:

- fenced retirement registration;
- non-blocking completed-entry collection;
- bounded waiting for live-device and injected-loss recovery;
- token-gated no-wait abandonment after authoritative device death;
- pending count, high-water mark, timeout, and allocation diagnostics.

The frame-retire queue and readback-retainer storage may remain internally specialized. The facade unifies enforcement and observability, not necessarily ownership representation.

Bounded drain snapshots eligible entries under the registry mutex, releases the mutex, waits on external fences, then reacquires the mutex to reconcile results. No registry mutex is held across a driver wait. Lock order must remain:

```text
worker/cache locks released → registry snapshot/reconcile → fence reset/rebuild
```

## Device-loss provenance and publication

The real-loss token and active loss epoch are published atomically under `GpuDeviceLossMonitor::m_epochMutex`. The lost state cannot become observable before its matching token is present for a real loss.

Backend-local detection authority owns minting. Ordinary code cannot name or call a public mint function. The Windows authority accepts only a failed `GetDeviceRemovedReason()` result. The Apple authority accepts only driver-authoritative `FrameOpDeviceLost` or `isDeviceLost()` observations. Test injection cannot mint a token.

Every authoritative Apple loss observation routes through the same atomic real-loss publication operation. Delayed or mismatched tokens are rejected and cannot cross a clear/reset boundary.

Injected loss remains tokenless and uses one total bounded live-device drain deadline. A submission
failure records where submission first failed but cannot reject later driver-authoritative proof:
one adapter reset may kill several owned device domains, so every current-authority dead domain is
published into the same loss epoch. Real-loss tokens carry device-domain identity, and one
token-gated registry scan releases only matching entries. Other live-device domains remain subject
to the bounded drain, which stops traversal and driver calls when its total deadline expires.

## Windows real-fault lane

The Windows fault lane is an opt-in test program isolated in a child process. It is never part of ordinary application execution. It reads TDR registry policy to reject non-recovering or unbounded configurations, but never changes registry settings.

### Capability gate

The parent records adapter identity and rejects destructive execution unless all conditions hold:

- `OLR_GPU_FAULT_LANE=1` is set;
- `OLR_GPU_FAULT_DEDICATED_RUNNER=1` explicitly asserts whole-adapter isolation;
- the backend is a real hardware D3D adapter, not WARP or Null;
- execution is not under an unsupported remote/virtual session;
- required D3D feature and fence support are present;
- TDR is configured for bounded recovery, with Microsoft's defaults used for absent values;
- a kill-on-close Job Object, child process, and watchdog can be created.

An unsupported environment reports a precise skip reason. Once explicitly enabled on supported hardware, a failed trigger, missing observation, timeout, or incorrect recovery is a test failure.

### Real fence-ordering probe

The child submits bounded long-running GPU work, signals a real fence value, and verifies:

- the value is initially incomplete;
- the retirement registry holds the surface while incomplete;
- completion advances to the value;
- collection releases the surface afterward;
- command submission signals only once and performs no CPU-side busy wait.

### Genuine device-removal probe

A dedicated child creates a real `PlaybackWorker` on the selected adapter and triggers a bounded GPU hang on that worker's exact D3D11 device. The hang shader reads the exact worker-cache surface whose fenced retain and destruction are observed, rather than an unrelated resource. The child verifies membership in the parent's named kill-on-close Job Object before device creation. The parent watchdog bounds total execution and terminates the contained process tree if recovery fails. The Job Object contains CPU processes, not already-submitted GPU work; the admitted TDR recovery policy and dedicated adapter runner are the GPU-side safety boundary.

The application-side harness must observe a real failed `GetDeviceRemovedReason()` or equivalent authoritative backend result. It then verifies:

- a real-loss token with the active generation was published;
- GPU generation advanced;
- no dead-device fence wait occurred;
- retained readbacks and frame surfaces were released;
- output produced no stale pre-loss frame;
- blackout/recovery stayed within the documented bound;
- CPU fallback or rebuilt GPU output resumed coherently.

The contained child keeps the real `PlaybackWorker`, hardware QRhi device, cached GPU frame with CPU
fallback, and output sink alive while it triggers TDR on that same device. The child flushes a
structured removal checkpoint before recovery and a second recovery result record afterward; the
parent merges successful JSONL checkpoints and nests any partial checkpoint beside raw stdout and
stderr on abnormal exit. Recovery evidence is
therefore taken from production worker orchestration, cache sanitization, generation rejection, and
post-loss output submission—not inferred from a reset of a separate device on the same adapter.

Logs preserve adapter identity, removal HRESULT, generation transition, fence values, registry counts, recovery timing, and child exit status.

## Performance constraints

Reliability enforcement must not become a performance trap.

Normal frame processing must add:

- no GPU wait;
- no additional fence signal;
- no global lock on submission;
- no allocation for common one-to-four-surface operations;
- no `std::function` or other type-erased callback;
- no duplicate retirement storage;
- no reference-count churn beyond the lifetime retains required by the existing protocol.

Shared-surface reads use an aliasing `shared_ptr` to the existing surface control block, so they add
no allocation and no extra native `AddRef`/`CFRetain`. The raw-surface encoder overload takes an
independently retained native reference because it has no shared surface owner; that retained
resource is ownership-safe even if a lease value survives its creating expression.

Performance measurements compare the parent commit and implementation commit using repeated runs and medians:

- CPU submission time per GPU operation;
- allocations per submission;
- fence signals per operation;
- registry mutex hold time;
- pending-retain high-water mark;
- compositor and readback throughput;
- end-to-end frame latency.

A repeated median regression above 2% blocks acceptance unless measurement noise is independently demonstrated. GPU command ordering must remain identical except where the old code was unsafe.

## Testing strategy

### Compile-time tests

- Direct `GpuSurface::nativeHandle()` access fails.
- Every D3D11 and Apple typed-backing accessor fails outside the scope protocol.
- Raw or typed backing extraction through `GpuReadLease` fails for ordinary callers.
- Lease copying, moving, or returning fails.
- Backend token minting is unavailable to ordinary translation units.

CMake `try_compile` tests are used where access-control SFINAE is not portable across supported compilers.

### Deterministic tests

- Synchronous callback access succeeds and cannot outlive the callback.
- Fenced scope signals once and registers every unique surface.
- Pre-submission cancellation registers nothing.
- Post-submission failure retains safely and records device-health failure.
- Completed collection, bounded timeout, real-loss abandonment, and high-water telemetry behave exactly.
- Clear/reset/publication races cannot install stale tokens.
- Registry waits occur without holding the registry mutex.

### Concurrency and platform tests

- TSan covers epoch publication, registry registration/collection, and device-loss races.
- ASan/UBSan cover all lease and registry unit tests.
- Windows and macOS compile every platform migration.
- The full unit, GPU, e2e, sanitizer, lint, and roadmap gates pass.
- The Windows hardware fault lane runs on an explicitly provisioned capable runner or documented local hardware before the program is considered complete.

## Review requirements

Each PR receives an independent fresh-context GPU concurrency/security review. PR #174 additionally receives a performance review based on measured results. PR #175 review includes containment, watchdog behavior, driver evidence, and confirmation that the fault lane cannot run accidentally.

## Non-goals

- Changing video formats, compositor quality, or output behavior.
- Adding normal-runtime fault injection to production builds.
- Automatically changing OS TDR policy.
- Replacing platform fences with polling or CPU synchronization.
- Combining unrelated roadmap initiatives into these PRs.
