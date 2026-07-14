# Transport Epoch Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every output-visible cache/playhead commit and play-epoch reset one indivisible protocol across seeks, armed cuts, and GPU recovery.

**Architecture:** Add one `PlaybackWorker::commitOutputStateLocked()` authority that validates, publishes, stores, and resets while `m_bufferMutex` remains held, returning only a post-lock dispatch obligation. Couple this C++ ownership boundary to deterministic barrier tests, compiled mutations, and a faithful bounded state model.

**Tech Stack:** C++17, Qt 6 Core/Test threading primitives, CMake/CTest, Python explicit-state model checker.

## Global Constraints

- A non-placeholder output lease may observe the committed generation only with the matching cache, playhead, GPU generation, and output play epoch.
- Hold-last is a real rendered frame and must satisfy the same invariant.
- An already-active pre-reset lease may finish; no later lease may start before a deferred reset applies.
- Lock order remains `m_mutex -> m_bufferMutex -> m_outputRuntimeMutex -> OutputRuntime::m_mutex`.
- `resetPlayEpoch()` must not wait for active dispatch completion while worker/cache locks are held; sink-local `discardPending()` locks remain permitted for immediate application.
- Every direct `m_committedGeneration.store()` outside the central primitive must be rejected by a source audit.
- Use deterministic barriers, not 250 ms scheduling assumptions.
- Use targeted `git add <paths>` and include `Co-Authored-By: Claude <noreply@anthropic.com>` in every commit.

---

## File Structure

- Modify `playback/playbackworker.h`: define `OutputCommit`, `OutputCommitResult`, dispatch obligation, and test barriers.
- Modify `playback/playbackworker.cpp`: implement the central commit primitive and migrate every commit family.
- Modify `playback/output/outputruntime.h` and `playback/output/outputruntime.cpp`: preserve F1 ordering and expose deterministic test state.
- Modify `tests/unit/tst_outputruntime.cpp`: deterministic F1/deferred-reset/hold-last tests.
- Modify `tests/unit/tst_playbackworker.cpp`: commit-family and early operator-PGM interleaving tests.
- Modify `docs/hardest-technical-challenges/transport_epoch_modelcheck.py`: faithful actors, phases, and per-family mutations.
- Create `tests/formal/transport_epoch_source_audit.py`: enforce C++ protocol ownership and F1 source order.
- Create `tests/mutations/transport_epoch_mutations.cmake`: compiled F1/F2 mutant targets.
- Create `tests/perf/tst_transportcommit_perf.cpp`: blocked-sink commit latency gate.
- Modify `tests/formal/CMakeLists.txt`, `tests/unit/CMakeLists.txt`, `tests/CMakeLists.txt`, and `docs/hardest-technical-challenges.md`.

### Task 1: Deterministically reproduce the early operator-PGM gap

**Files:**
- Modify: `playback/playbackworker.h`
- Modify: `playback/playbackworker.cpp`
- Modify: `tests/unit/tst_playbackworker.cpp`

**Interfaces:**
- Produces: test-only `setOutputCommitBarrierForTest()` around the current early commit/dispatch boundary.
- Consumes: `tryCompleteOperatorSeekFromCurrentOutputCache()` and `dispatchPgmAfterSeekCommit()`.

- [ ] **Step 1: Add a barrier-controlled failing test**

Install a test-only callback immediately after the early path stores `m_committedGeneration` and publishes the cache but before `dispatchPgmAfterSeekCommit()`. Pause there, let a background output tick capture a lease, and assert it cannot submit the pre-seek frame or hold-last identity.

```cpp
void TestPlaybackWorker::earlyOperatorCommitCannotExposeStaleEpoch() {
    CommitBarrier barrier;
    worker.setOutputCommitBarrierForTest(&barrier);
    auto seek = std::async(std::launch::async,
                           [&] { return worker.seekToAndWaitForPgm(1200, 1, 2000); });
    QVERIFY(barrier.waitUntilEntered(2000));
    runtime.dispatchDueTicksForTest(1200);
    barrier.release();
    const auto result = seek.get();
    QVERIFY(result.submittedPgm);
    QCOMPARE(pgmSink.frames.last().identity.sampledPlayheadMs, qint64(1200));
    QCOMPARE(pgmSink.frames.last().identity.sourcePtsMs, qint64(1200));
    QVERIFY(!pgmSink.frames.last().identity.videoPlaceholder);
}
```

- [ ] **Step 2: Run the single test and prove it is red**

Run: `cmake -S . -B build/transport -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/mingw_64 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DOLR_GPU_PIPELINE=ON -DOLR_WERROR=OFF -DOLR_BUILD_TESTS=ON -DOLR_FFMPEG_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/ffmpeg -DOLR_SRT_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/srt`

Run: `cmake --build build/transport --target tst_playbackworker && build/transport/tests/unit/tst_playbackworker.exe earlyOperatorCommitCannotExposeStaleEpoch`

Expected: FAIL because the background lease can run after the generation store but before the epoch reset.

- [ ] **Step 3: Commit the red regression test**

```powershell
git add playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_playbackworker.cpp
git commit -m "test(playback): reproduce early seek epoch gap" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Centralize output-visible commit authority

**Files:**
- Modify: `playback/playbackworker.h`
- Modify: `playback/playbackworker.cpp`
- Modify: `tests/unit/tst_playbackworker.cpp`

**Interfaces:**
- Produces: `commitOutputStateLocked(const OutputCommit&) -> OutputCommitResult`.
- Consumes: `CommitGate`, output-cache helpers, and `resetOutputPlayEpoch()`.

- [ ] **Step 1: Add focused primitive tests**

Test a rejected stale generation, successful publish, displayable fallback, GPU-generation store, cache-guard update, reset count, and returned PGM/preview dispatch obligation. Assert no state changes when validation fails.

- [ ] **Step 2: Verify the primitive tests fail before the API exists**

Run: `cmake --build build/transport --target tst_playbackworker && ctest --test-dir build/transport -R '^tst_playbackworker$' --output-on-failure`

Expected: build fails on the missing `OutputCommit` API.

- [ ] **Step 3: Define the typed protocol**

Add these private types:

```cpp
enum class OutputCacheAction : uint8_t { Keep, Publish, MergeStagingAndPublish };
enum class PostCommitDispatch : uint8_t { None, Output, Preview, PgmCritical };

struct OutputCommit {
    qint64 playheadMs = 0;
    uint64_t seekGeneration = 0;
    uint64_t gpuGeneration = 0;
    OutputCacheAction cacheAction = OutputCacheAction::Keep;
    OutputCoverageMode coverageMode = OutputCoverageMode::StrictSeek;
    bool requireCurrentSeek = true;
    bool clearSeekTarget = false;
    bool guardPlayheadCache = false;
    PostCommitDispatch dispatch = PostCommitDispatch::None;
};

struct OutputCommitResult {
    bool committed = false;
    qint64 committedPlayheadMs = 0;
    uint64_t committedGeneration = 0;
    PostCommitDispatch dispatch = PostCommitDispatch::None;
};
```

`commitOutputStateLocked()` requires `m_bufferMutex` to be held and performs: validation, cache action, playhead/visible/GPU stores, guard/seek-target updates, the release-store to `m_committedGeneration`, and `resetOutputPlayEpoch()` before returning. Its result carries no raw cache pointer.

- [ ] **Step 4: Migrate request-time cache reuse, live-start fallback, reuse reposition, and full reposition**

Replace each direct store/reset block with one `OutputCommit` call. Move `dispatchPgmAfterSeekCommit()`, `refreshPreviewAfterSeekCommit()`, and `refreshOutputAfterSeekCommit()` after all worker/cache locks are released. Remove `runtime->resetPlayEpoch()` from `dispatchPgmAfterSeekCommit()`; it may retry dispatch but may not mutate the epoch.

- [ ] **Step 5: Run focused tests including the red regression**

Run: `cmake --build build/transport --target tst_playbackworker tst_outputruntime && ctest --test-dir build/transport -R 'tst_(playbackworker|outputruntime)' --output-on-failure`

Expected: early operator regression and migrated seek/reposition tests pass.

- [ ] **Step 6: Commit the central primitive**

```powershell
git add playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_playbackworker.cpp
git commit -m "fix(playback): centralize output epoch commits" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: Migrate armed-cut and GPU recovery commit families

**Files:**
- Modify: `playback/playbackworker.cpp`
- Modify: `tests/unit/tst_playbackworker.cpp`
- Modify: `tests/unit/tst_gpu_devicelost_worker.cpp`

**Interfaces:**
- Produces: zero direct committed-generation stores outside `commitOutputStateLocked()`.
- Consumes: the Task 2 commit protocol.

- [ ] **Step 1: Add one identity/reset assertion per remaining family**

Cover armed-cut promotion, device-loss recovered cache, memory-pressure recovered cache, and graph-generation recovery. Each test records submitted frame identity, committed playhead, placeholder/hold-last state, seek/GPU generations, and reset count.

- [ ] **Step 2: Run tests and identify uncentralized behavior**

Run: `cmake --build build/transport --target tst_playbackworker tst_gpu_devicelost_worker && ctest --test-dir build/transport -R 'tst_(playbackworker|gpu_devicelost_worker)' --output-on-failure`

Expected: at least one new atomic-reset assertion fails on a remaining direct store path.

- [ ] **Step 3: Route every remaining output-visible commit through the primitive**

For recovery paths that do not advance seek generation, pass the currently committed seek generation explicitly and set `requireCurrentSeek = false`; still reset the epoch under `m_bufferMutex`. Keep non-output GPU bookkeeping stores outside only when they cannot open the `CommitGate`; rename those fields/comments so the source audit cannot confuse them with an output commit.

- [ ] **Step 4: Prove no direct generation stores remain**

Run: `rg -n "m_committedGeneration\.store" playback/playbackworker.cpp`

Expected: exactly one result, inside `commitOutputStateLocked()`.

- [ ] **Step 5: Run all worker/recovery tests**

Run: `cmake --build build/transport --target tst_playbackworker tst_gpu_devicelost_worker && ctest --test-dir build/transport -R 'tst_(playbackworker|gpu_devicelost_worker)' --output-on-failure`

Expected: all selected tests pass.

- [ ] **Step 6: Commit remaining migrations**

```powershell
git add playback/playbackworker.cpp tests/unit/tst_playbackworker.cpp tests/unit/tst_gpu_devicelost_worker.cpp
git commit -m "fix(playback): atomically commit cut and recovery epochs" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Deterministic F1 and deferred-reset semantics

**Files:**
- Modify: `playback/output/outputruntime.h`
- Modify: `playback/output/outputruntime.cpp`
- Modify: `tests/unit/tst_outputruntime.cpp`

**Interfaces:**
- Produces: tested F1 order and deterministic active-dispatch reset barriers.
- Consumes: `m_configGeneration`, `m_pendingPlayEpochReset`, and `applyPendingDispatchMutationsLocked()`.

- [ ] **Step 1: Replace scheduler windows with entered/release barriers**

Refactor `RuntimeResetDuringSubmitSink` so `submit()` signals `entered`, waits on `release`, and records whether reset returned before release. Use a two-second timeout only for diagnostic failure.

- [ ] **Step 2: Add failing snapshot-before-lease and multiple-reset tests**

Pause after snapshot provider capture but before lease recheck; call `resetPlayEpoch()`; prove the snapshot is rejected. While one lease is active, issue three resets and prove the active lease completes, one coalesced reset applies, and the next lease sees the cleared epoch.

- [ ] **Step 3: Run OutputRuntime tests before changes**

Run: `cmake --build build/transport --target tst_outputruntime && ctest --test-dir build/transport -R '^tst_outputruntime$' --output-on-failure`

Expected: new deterministic assertions expose any missing capture/recheck/deferred behavior.

- [ ] **Step 4: Preserve the exact F1 implementation order**

Keep this order in `resetPlayEpoch()`:

```cpp
QMutexLocker locker(&m_mutex);
++m_configGeneration;
if (m_dispatchActive) {
    m_pendingPlayEpochReset = true;
    return;
}
m_dispatcher.resetPlayEpoch();
refreshCachedStatsLocked();
```

Ensure `applyPendingDispatchMutationsLocked()` applies the pending reset before clearing the dispatch-active barrier or allowing another lease. Update comments to say “non-waiting with respect to an active dispatch,” not universally nonblocking.

- [ ] **Step 5: Run deterministic runtime tests repeatedly**

Run: `1..20 | ForEach-Object { ctest --test-dir build/transport -R '^tst_outputruntime$' --output-on-failure; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: twenty clean passes with no timing-dependent skip/failure.

- [ ] **Step 6: Commit runtime semantics**

```powershell
git add playback/output/outputruntime.h playback/output/outputruntime.cpp tests/unit/tst_outputruntime.cpp
git commit -m "test(output): make epoch reset interleavings deterministic" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5: Faithful model, source audit, and compiled mutations

**Files:**
- Modify: `docs/hardest-technical-challenges/transport_epoch_modelcheck.py`
- Create: `tests/formal/transport_epoch_source_audit.py`
- Create: `tests/mutations/transport_epoch_mutations.cmake`
- Modify: `tests/formal/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `playback/playbackworker.cpp`
- Modify: `playback/output/outputruntime.cpp`

**Interfaces:**
- Produces: bounded model actors for every commit family and source/compiled mutation gates.
- Consumes: exact Task 2/4 source markers and test-only mutation definitions.

- [ ] **Step 1: Add the early operator and production-order actors to the model**

Represent distinct actors for published reuse, live fallback, reuse reposition, full reposition, early operator completion, armed cut, device-loss recovery, and memory-pressure recovery. Split dispatch into capture, provider call, identity selection including hold-last, generation recheck, lease, and completion. Model config capture before provider execution and deferred reset during an active lease.

- [ ] **Step 2: Add one mutation per F1/F2 owner**

Accepted modes must include `fixed`, `mut_f1`, and one `mut_f2_<family>` for each actor. `all` succeeds only if fixed proves the invariant and every mutant emits a concrete counterexample containing actor, state, sampled playhead, committed playhead, generation, and rendered identity.

- [ ] **Step 3: Run the model differential gate**

Run: `python docs/hardest-technical-challenges/transport_epoch_modelcheck.py all`

Expected: fixed proves; every mutation refutes; exit code is zero only for the differential result.

- [ ] **Step 4: Add the source audit**

Parse `playbackworker.cpp` and require the sole `m_committedGeneration.store` to be lexically inside `commitOutputStateLocked`. Parse `outputruntime.cpp` and require `++m_configGeneration` before the `m_dispatchActive` branch. Fail with the exact offending line.

- [ ] **Step 5: Add compiled F1/F2 mutations**

Guard the two load-bearing statements with test-only definitions `OLR_MUTATE_SKIP_CONFIG_GENERATION` and `OLR_MUTATE_SKIP_COMMIT_EPOCH_RESET`. Build dedicated test libraries/executables with one definition at a time; each CTest passes only when its deterministic regression test fails under the mutant. Production builds define neither macro.

- [ ] **Step 6: Run formal and mutation gates**

Run: `ctest --test-dir build/transport -R 'transport_epoch_(modelcheck|source_audit|mutation_)' --output-on-failure`

Expected: model, source audit, and both compiled mutation families pass differentially.

- [ ] **Step 7: Commit proof coupling**

```powershell
git add docs/hardest-technical-challenges/transport_epoch_modelcheck.py tests/formal/transport_epoch_source_audit.py tests/mutations/transport_epoch_mutations.cmake tests/formal/CMakeLists.txt tests/CMakeLists.txt playback/playbackworker.cpp playback/output/outputruntime.cpp
git commit -m "test(playback): couple epoch proof to production source" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 6: Commit latency, documentation, and full verification

**Files:**
- Create: `tests/perf/tst_transportcommit_perf.cpp`
- Create: `tests/perf/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/hardest-technical-challenges.md`

**Interfaces:**
- Produces: blocked-sink latency evidence and accurate public claims.
- Consumes: central commit protocol and runtime barriers.

- [ ] **Step 1: Add a blocked-sink latency test**

Block an active sink submission, invoke a cache commit on another thread, and assert `commitOutputStateLocked()` plus reset returns within 20 ms while the submission remains blocked. Release the sink and separately measure operator PGM completion outside worker/cache locks.

- [ ] **Step 2: Run the latency test**

Run: `cmake --build build/transport --target tst_transportcommit_perf && ctest --test-dir build/transport -R '^tst_transportcommit_perf$' --output-on-failure`

Expected: PASS; commit latency is independent of the blocked submission.

- [ ] **Step 3: Update documentation and remove stale lock claims**

Document represented actors, bounded model assumptions, source/compiled coupling, exact lock order, active-lease completion semantics, and benchmark evidence. Remove any comment claiming `resetPlayEpoch()` always waits for dispatch idle.

- [ ] **Step 4: Run complete verification**

```powershell
cmake --build build/transport
ctest --test-dir build/transport -L unit --output-on-failure
ctest --test-dir build/transport -R '^transport_epoch_' --output-on-failure
ctest --test-dir build/transport -R '^tst_transportcommit_perf$' --output-on-failure
git diff --check
```

Expected: complete build/unit/formal/mutation/performance gates pass and diff check is silent.

Run the existing CI sanitizer matrix with the deterministic worker/runtime tests included in both `-DOLR_SANITIZER="address;undefined"` and `-DOLR_SANITIZER="thread"` legs. Expected: both legs pass without timing skips or new suppressions.

- [ ] **Step 5: Commit final evidence**

```powershell
git add tests/perf/tst_transportcommit_perf.cpp tests/perf/CMakeLists.txt tests/CMakeLists.txt .github/workflows/ci.yml docs/hardest-technical-challenges.md
git commit -m "test(playback): gate epoch commit latency" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 7: Independent concurrency review and merge-ready handoff

**Files:**
- Modify only paths required by reproduced review findings.

**Interfaces:**
- Consumes: complete branch and all machine evidence.
- Produces: review-clean PR update; no automatic merge.

- [ ] **Step 1: Request a fresh-context concurrency review**

Require review of early operator PGM, every central-commit caller, F1/deferred ordering, hold-last identity, `m_mutex`/buffer/runtime lock order, model fidelity, and mutation/source coupling.

- [ ] **Step 2: Fix findings test-first**

For every Critical/Important finding, reproduce it with a deterministic failing test or model counterexample, implement the smallest design-consistent fix, and rerun the focused plus full gates.

- [ ] **Step 3: Commit review corrections as focused changes**

Stage only the files belonging to each reproduced finding and commit them with the required co-author trailer. Confirm `git status --short` is clean before pushing.

- [ ] **Step 4: Push with hooks and stop before merge**

```powershell
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin fix/transport-epoch-reanchor
```

Expected: pre-push succeeds. Update/open the PR with model state count, mutation results, latency evidence, and test matrix, then hand it to the user without auto-merging.

## Fix Wave: Task 4 follower registration barrier

**Review finding:** The deferred-reset helper signaled `followerStarted` before entering
`dispatchImmediate()`, so the sink could be released and the reset applied before the follower
registered. The assertions could therefore pass without proving that the follower was queued
behind the active lease.

- [x] Reproduce the false positive with a deterministic temporary gate that holds the follower
  before `dispatchImmediate()`; confirm both real-frame and hold-last cases still pass the old
  started-thread assertion.
- [x] Replace the started-thread signal with
  `waitForImmediateDispatchRequestsForTest(2, 2000)`. The completed seed dispatch resets the live
  request count to zero and the blocked active request supplies the baseline of one, so reaching
  two proves follower registration before release.
- [x] Keep the timeout diagnostic-only and release the sink before joining every spawned thread;
  do not add scheduler sleeps or change production synchronization/lifetime behavior.
- [x] Run both corrected cases, the complete runtime test 20 times, the selected transport matrix,
  formatting/diff checks, and a fresh review of `78b4e018..HEAD`.
- [x] Commit only the test helper and this fix-wave record with the required co-author trailer; do
  not push.

## Fix Wave: Task 5 production-coupled mutation and source audit

**Review findings:** The compiled mutation wrapper accepted a mutant kill without first proving
the same selector passed against unmutated production. The source audit did not apply C/C++
escaped-newline splicing, treated any lexically present F1 increment as effective, and did not
require an unconditional F2 reset immediately after the committed-generation store. The model's
523-state verdict also did not say that the count was summed across independent scenario graphs.

- [x] Capture RED evidence: the old source audit accepted an escaped-newline direct store outside
  `commitOutputStateLocked`, and the old mutation wrapper accepted F1 while its paired baseline was
  meta-mutated with the same omission and remained RED.
- [x] Compile dedicated unmutated F1/F2 baseline executables and require the focused selector,
  expected PASS name, and zero-failure QtTest totals before accepting the corresponding mutant
  kill. Always execute the mutant too so a broken-baseline rejection records whether it still died
  for the expected assertion.
- [x] Add F1/F2 meta-mutations that use the compiled mutant as the nominal production baseline;
  require the outer gate to see both `unmutated baseline failed` and `mutant independently killed`.
- [x] Normalize LF and CRLF escaped-newline splices with original-offset provenance before lexical
  matching. Ignore comments, strings, declarations, and preprocessor macro definitions while
  accepting valid whitespace/comment variants at the central owner.
- [x] Require F1 to be an unconditional, reachable, production-active top-level increment before
  the active-dispatch branch. Require exactly one production-active, unconditional F2 reset as the
  next top-level statement after the sole central committed-generation store; reject absent,
  conditional, unreachable, duplicate, and misordered forms with the offending source line.
- [x] Clarify that the fixed model's 523 reachable states are aggregated across eleven independent
  scenario graphs rather than belonging to one coupled graph.
- [x] Run GPU-on and GPU-off baseline/mutant/meta/source/model gates (6/6 each), the normal GPU-on
  `tst_outputruntime` and `tst_playbackworker` targets (2/2), the normal GPU-off
  `tst_outputruntime` target (1/1; GPU-off omits `tst_playbackworker`), all model mutants, Python
  compile checks, roadmap audit, and diff/line-ending checks. Do not push.

## Fix Wave 2: Task 5 production reachability barriers

**Review finding:** The top-level source parser discarded complete braced statements and the F1
reachability check matched only the exact statement `return;`. A production-only compound return
could therefore bypass both F1 and F2 while the compiled baselines, built with `OLR_UNIT_TEST`,
removed the adversarial branch.

- [x] Capture RED evidence for production-only braced returns before F1 and between the F2 store
  and reset, plus nested `if`/`switch` transfers and `goto`, `throw`, and `co_return` variants.
- [x] Preserve complete top-level compound/control statements and reject explicit non-fallthrough
  transfers whenever their preprocessor branch can be active in production. Keep comments,
  test-only branches, and transfer-free scopes accepted.
- [x] Add a production-representative baseline that compiles the guarded source without
  `OLR_UNIT_TEST` and runs the public F1 lease-recheck scenario; retain the paired focused
  baseline/mutant/meta gates for F1 and F2.
- [x] Run GPU-on/off proof gates, meta-mutants, source adversarial tests, model checks, full tests,
  roadmap audits, formatting/diff checks, and a fresh review of `187f72ea..HEAD`. Do not push.
