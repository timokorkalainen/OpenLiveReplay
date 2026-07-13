# GPU Capability and Retirement Hardening Design

## Purpose

The #173–#175 stack closes direct base-surface handle access and materially improves real device-loss recovery, but it does not yet make asynchronous ownership and loss authority structurally safe. Handle extraction, tracking, and submission remain separate; copied dead-device tokens can outlive their authority; fence timeline provenance is incomplete; and the current global registry can become a hot-path lock/scan bottleneck.

This design completes Challenge 3 without changing its locked boundaries:

- `GpuSurface::nativeHandle()` remains protected and `GpuReadLease` remains its only friend.
- `retainUntilFenceRetired()` and `pendingFenceValue()` remain public monotonic watermark operations.
- All native backing acquisition continues through `GpuSyncReadScope`.

## Capability model

### Synchronous access

The locked lease-returning API remains public: synchronous callers use `GpuSyncReadScope scope; auto lease = scope.read(surface);`, access `lease.nativeHandle()`, and call `scope.complete()` after the native operation finishes. Add callback-form `GpuSyncReadScope::withRead(surface, fn)` as the preferred production convenience, with `complete()` performed by the wrapper. Existing direct-read tests remain valid and continue to prove that base and derived surfaces cannot bypass the lease.

The scope has explicit active/completed state. A lease is bound to that state, rejects handle access after completion in checked builds, and cannot be copied or moved. Scope exit without completion is a contract failure in checked builds and emits a rate-limited diagnostic in release builds; the lease owner still remains alive until lease destruction. This makes accidental scope misuse observable without claiming that C++ can revoke a copied raw pointer.

The callback contract is explicitly synchronous. Raw pointer escape cannot be prevented against malicious code once a platform API accepts `void*`, so production asynchronous paths do not expose that pointer: backend-local adapters unwrap the lease and execute the native call. Direct `read()` remains available for the intentionally synchronous API required by the locked design, and a source audit limits production uses to reviewed synchronous adapters.

### Fused asynchronous submission

`GpuOpScope` exposes backend submission entry points that fuse:

1. Surface collection.
2. Fence/device/timeline compatibility validation.
3. Retirement-capacity reservation.
4. `GpuSyncReadScope` acquisition inside a privileged backend adapter.
5. Native submission.
6. Fence signal and durable retirement publication.

Callers describe the operation using typed backend commands or callbacks that receive an opaque backend view, not `void*`. Only the D3D11/Metal/QRhi adapter can unwrap the native handle. There is no public `track()` followed by independent `submit()` path in production. Deleting ownership registration therefore deletes the submission API implementation itself and is caught by tests.

The common one-to-four-surface operation remains inline and allocation-free. Overflow capacity is reserved before any native call can submit work.

## Submission failure safety

Submission adapters are statically `noexcept` and return `NotSubmitted`, `Submitted`, or `SubmittedWithError`. Platform calls that report errors do so through the typed outcome rather than exceptions.

Before submission, the scope owns a prepared retirement record for every surface. After the driver may have accepted work, ownership moves only to:

- the signaled per-fence retirement queue; or
- a preallocated quarantine owned by the device domain when signaling fails.

No allocation, container growth, or throwing operation occurs after possible submission. Quarantine is released only by authoritative dead-domain abandonment or orderly device shutdown after synchronization.

## Fence and timeline provenance

Every retirement record contains the exact `shared_ptr<GpuFence>`, fence-instance identity, device-domain identity, GPU generation, signal value, and surface owner. `GpuOpScope` rejects a surface whose backing is incompatible with its fence before submission.

Surface-global pending watermarks remain available for their existing monotonic purposes but are never paired with an arbitrary fence for async retirement. `GpuFrameRetireQueue` and Windows readback consumers migrate to explicit `(fence, value, domain, generation)` tickets.

`GpuRetireRegistry::registerRetire()` becomes internal to the prepared submission/quarantine machinery; arbitrary public fence/surface pairing is removed.

## Epoch-bound loss authority

`DeadDeviceToken` gains an authority-epoch field but is not itself accepted by the registry. No-wait abandonment is performed through `GpuDeviceLossMonitor::withValidatedDeadDomains(fn)`:

1. Acquire `m_epochMutex` and the process recovery-leader lock in the documented order.
2. Verify the monitor is still lost, rebuild has not begun, and every domain proof matches current authority epoch and loss generation.
3. Invoke the registry abandonment callback while authority remains stable.
4. Release registry state before releasing the recovery leader.

`beginRebuild()` invalidates authority before replacement devices or retirement records can register. Registry code never calls the monitor while holding a shard lock, preventing inverse lock order. Copied tokens cannot authorize release directly.

A process-wide recovery coordinator elects one leader per loss generation. Other workers join the result rather than independently clearing/rebuilding shared authority. Multiple proven dead domains from one adapter reset remain supported; unmatched live domains use one total bounded wait.

## Retirement storage and performance

Replace the single process-wide `QVector` with sharded queues keyed by exact fence instance/device domain. Each shard uses pooled fixed-capacity blocks:

- common registration publishes prepared nodes under a short shard lock with no allocation;
- completion queries each distinct fence once outside shard locks;
- draining unlinks completed nodes in one pass without copying the entire registry or building a `QSet`;
- loss abandonment visits only matching domain shards;
- diagnostics use atomics and per-shard counters.

The implementation must ship microbenchmarks for compositor and import/readback paths against #173 and #174 heads. Release median and p95 steady-state overhead must remain within 2%; no process-wide lock or common-path allocation is allowed.

## Negative compilation and mutation tests

Replace probes that fail through nonexistent APIs with exact contract tests:

- base/derived `nativeHandle()` access remains inaccessible while the public synchronous lease pattern compiles;
- arbitrary code cannot unwrap an opaque submitted view;
- async submission cannot be formed without surface ownership and compatible fence evidence;
- arbitrary registry fence/surface pairing is inaccessible;
- token construction and direct abandonment are inaccessible;
- stale authority, wrong generation, wrong domain, and wrong fence timeline are runtime-rejected.

Each compile-fail case has a paired compile-pass control and validates the intended capability concept, not an arbitrary compiler failure. Mutation builds remove compatibility validation, loss-generation validation, durable retirement publication, and quarantine transfer; named tests must fail.

## Device-loss and Apple verification

Add deterministic multi-worker tests for tokenless-first upgrade, concurrent recovery leadership, rebuild invalidation, reused Apple `MTLDevice` identity, multiple dead domains, and unmatched live-domain bounded waits.

Apple tests cover CFRetain/CFRelease balance, command-buffer completion placement, typed adapter lifetime, and old-device/new-queue identity. macOS CI compiles and executes the platform tests; Windows static inspection is not treated as the gate.

## Fault-lane scope

The real Windows lane remains manual, dedicated-runner-only, TDR-policy-gated, and Job-contained for CPU processes. Capability admission also creates the exact NV12 plane views, fence, shader, and worker cache resources needed later, without dispatching the destructive shader.

Add mutation jobs for authoritative-upgrade removal, recovery abandonment removal, stale-frame acceptance, and worker-output non-resumption. The destructive lane continues in `RelWithDebInfo`; it does not claim ASan coverage. The same recovery state machine runs non-destructively under ASan/UBSan/TSan with injected and fake authoritative domains. Evidence reports discrete post-loss submissions and recovery time; it does not claim continuous zero-gray output unless a continuous sampler is added.

Virtual-adapter detection remains defense in depth rather than an exhaustive guarantee. The dedicated-runner attestation and documented adapter-wide blast radius remain mandatory.

## Documentation and cleanup

Narrow claims to what exact tests falsify, document the synchronous pointer boundary honestly, and remove trailing whitespace/blank-line `git diff --check` failures in touched plan files. Failed-signal quarantine ownership and operator diagnostics must be documented.

## Acceptance

The stack is ready only when asynchronous ownership-bypass examples no longer compile through production APIs, the locked synchronous lease pass-control still compiles, stale tokens cannot abandon new records, wrong-fence tests fail before submission, allocation/exception mutations preserve ownership, multi-worker recovery is serialized, Apple CI passes, the performance gate meets 2%, the non-destructive sanitizer matrix and real TDR lane pass, and an independent fresh-context GPU concurrency/security review finds no Critical or Important issue.
