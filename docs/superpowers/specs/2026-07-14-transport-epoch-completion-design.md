# Transport Epoch Completion Design

## Purpose

PR #172 implements a sound generation invalidation barrier and repairs most commit/reset gaps, but the early operator-PGM cache commit still publishes an open gate before resetting the output epoch. The abstract model also is not coupled strongly enough to production source. This design completes Challenge 1 with one commit primitive, faithful model actors, and deterministic compiled interleaving tests.

## Invariant

For every output lease that renders a non-placeholder frame while `committedGeneration == seekGeneration`, the sampled media time and the output-visible committed playhead must refer to the same play epoch within one frame. Hold-last counts as rendering a real frame and must satisfy the same invariant.

An output lease already active when a reset is requested may finish against its captured pre-reset state. No later lease may observe the new committed generation/cache before the reset is installed.

## Atomic commit architecture

Introduce one `PlaybackWorker` primitive used only while `m_bufferMutex` is held:

`commitOutputStateLocked(const OutputCommit&) -> OutputCommitResult`

`OutputCommit` carries the target playhead, seek generation, GPU generation, cache publication choice, cache-guard state, and dispatch intent. The primitive performs, in this order while the buffer lock remains held:

1. Validate cache coverage and the `CommitGate` generation.
2. Publish or swap the selected output cache.
3. Store committed playhead, visible playhead, GPU generation, and committed seek generation.
4. Reset the output play epoch, which increments `OutputRuntime::m_configGeneration` or records a deferred reset when a lease is active.
5. Return a typed post-lock dispatch obligation.

No call site may publish `m_committedGeneration` directly. A source-level rule will enforce this ownership boundary.

Migrate all output-visible commit families:

- request-time published-cache reuse;
- live-start displayable fallback;
- worker reuse reposition;
- full reposition;
- early operator-PGM completion;
- armed-cut cache swap;
- GPU device-loss recovered cache;
- GPU memory-pressure recovered cache.

Post-commit helpers perform immediate dispatch only. They cannot reset the epoch, publish caches, or open the gate.

## OutputRuntime semantics

Retain F1: every `resetPlayEpoch()` increments configuration generation before deciding whether reset application is immediate or deferred. A captured-but-not-leased snapshot therefore fails its recheck. An active lease completes, then `applyPendingDispatchMutationsLocked()` applies the reset before clearing the dispatch-active barrier.

Document the operation precisely as non-waiting with respect to an active dispatch, not universally nonblocking: sink `discardPending()` may take sink-local delivery locks when immediate application is safe.

Reset coalescing remains a boolean because no intervening lease can start before pending mutations apply. Tests must prove this ordering.

## Formal model fidelity

Extend `transport_epoch_modelcheck.py` with distinct actors for every commit family, including early operator-PGM completion. Model the production order exactly:

- capture `configGeneration` before invoking the snapshot provider;
- allow the snapshot provider to fire a cut/reset;
- determine actual output identity from epoch-sampled playhead;
- model hold-last converting a placeholder into a previously submitted real frame;
- split immediate dispatch into capture, snapshot, recheck, lease, and completion phases;
- model deferred reset while a lease is active.

Add mutations for deletion of F1 and for omission of the atomic reset in each commit family. Fixed must prove; every mutation must produce a concrete counterexample.

## Source coupling

The Python proof alone cannot detect a deleted C++ call. Add a source-level protocol audit that parses the small set of allowed committed-generation stores and requires them to occur inside `commitOutputStateLocked`. The audit also requires `resetPlayEpoch()` to increment `m_configGeneration` before the active-dispatch branch.

Pair this with compiled mutation targets where F1 and commit reset are disabled by narrow test-only compile definitions. The deterministic tests must fail under each mutation and pass in production configuration.

## Deterministic compiled tests

Add barriers instead of scheduler windows:

- Snapshot captured before reset, paused before lease: reset must invalidate it and no stale frame may submit.
- Reset during active submit: reset returns without waiting for submit completion; the active lease finishes and the next lease observes a cleared epoch.
- Early operator-PGM commit while playing: pause between commit publication and post-lock dispatch and prove no background lease can render stale/hold-last output.
- Published-cache reuse, full reposition, live-start fallback, armed cut, and both GPU recovery commits assert atomic reset and frame identity.
- Multiple resets during one active lease coalesce without losing the required reset.

Tests assert submitted frame identity, sampled playhead, placeholder/hold-last status, configuration generation, and reset application—not only return timing or reset counters.

The `RuntimeResetDuringSubmitSink` test uses explicit entered/release barriers with a two-second diagnostic timeout. No 250 ms scheduling assumption remains.

## Locking and latency

The required order remains `m_mutex -> m_bufferMutex -> m_outputRuntimeMutex -> OutputRuntime::m_mutex`. `resetPlayEpoch()` must never wait for `m_dispatchActive` while worker/cache locks are held. Snapshot construction releases `m_outputRuntimeMutex` before taking `m_bufferMutex`; tests and comments preserve this rule.

Add a seek-commit latency benchmark with an intentionally blocked output sink. Commit latency must remain independent of the blocked submission, while immediate PGM completion may wait outside worker/cache locks according to its operator timeout.

## Documentation cleanup

Remove stale comments claiming `resetPlayEpoch()` calls `waitForDispatchIdleLocked()`. Document the bounded model assumptions and avoid saying the model is a source-level proof. The PR may claim the invariant only for actors represented in both the model and compiled mutation suite.

## Acceptance

The branch is ready only when the early-operator counterexample is red before implementation and green after, every commit family uses the central primitive, all model mutations falsify, compiled mutation tests fail as intended, sanitizer/thread suites pass, latency remains within budget, and independent fresh-context concurrency review finds no Critical or Important issue.
