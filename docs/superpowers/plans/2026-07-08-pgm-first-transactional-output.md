# PGM-First Transactional Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make paused seek, step, jog, and scrub commands complete only after the
correct GPU-rendered PGM frame has been submitted to the required PGM output lane.

**Architecture:** Add a PGM-first dispatch lane with submission evidence, then add
a waitable playback-worker transaction that wakes the paused worker and completes
from PGM submit evidence. WebSocket and test controls wait on that transaction;
Qt preview follows after PGM and cannot delay command completion.

**Tech Stack:** C++17, Qt/QThread/QMutex/QWaitCondition, QRhi GPU path,
OpenLiveReplay output buses, QtTest, macOS app E2E WebSocket driver, NDI marker
probe.

## Global Constraints

- Do not commit any changes until the user explicitly grants permission again.
- PGM is the authoritative output; Qt preview is non-authoritative and must never
  block PGM command completion.
- PGM composition and future PGM processing must remain GPU-owned, including
  selected-source PGM.
- macOS app E2E is the first full-system gate; iOS/iPadOS validation follows only
  after the macOS gate is reliable.
- Cold seeks must jump more than five seconds from the previous playhead.
- Hard latency gate remains no PGM NDI marker sample over 25 ms; the target design
  should aim for under 15 ms command-to-PGM-submit on fast paths.
- A single wrong marker, stale frame, placeholder, or latency miss fails the strict
  run.
- Use `apply_patch` for manual edits.

---

## File Structure

- Modify `playback/output/outputdispatcher.h/.cpp`: add dispatch request/report
  types, lane filtering, and PGM submission evidence.
- Modify `playback/output/outputruntime.h/.cpp`: expose PGM-first immediate
  dispatch and return dispatch evidence.
- Modify `playback/playbackworker.h/.cpp`: add operator seek transaction state,
  paused-worker wake, and PGM dispatch completion.
- Modify `uimanager.h/.cpp`: expose waitable operator seek/jog wrappers for
  external controls while keeping QML methods usable.
- Modify `websocket/uimanagercontroladapter.cpp`: wait for transaction completion
  before ACK for seek/step/jog commands.
- Modify `tests/unit/tst_outputdispatcher.cpp`, `tests/unit/tst_outputruntime.cpp`,
  and `tests/unit/tst_playbackworker.cpp`: lock in lane ordering, transaction
  completion, supersession, and worker wake behavior.
- Modify `tests/e2e/macos_app_driver.py`: correlate command id, ACK timing, and
  PGM marker timing after transaction changes.
- Modify `docs/frame-accurate-scrub-testing.md`: document the PGM-first
  transaction oracle and expected failure diagnostics.

---

### Task 1: PGM Dispatch Lane Evidence

**Files:**
- Modify: `playback/output/outputdispatcher.h`
- Modify: `playback/output/outputdispatcher.cpp`
- Test: `tests/unit/tst_outputdispatcher.cpp`

**Interfaces:**
- Produces:
  - `enum class OutputDispatchLane { All, PgmCritical };`
  - `struct OutputDispatchRequest { OutputDispatchLane lane; OutputBusId requiredBus; OutputTargetKind requiredKind; qint64 requiredPlayheadMs; bool requireNonPlaceholder; };`
  - `struct OutputSubmittedFrame { OutputTargetAssignment assignment; OutputFrameIdentity identity; bool submitted; qint64 submitNs; };`
  - `struct OutputDispatchReport { OutputDispatchStats stats; QList<OutputSubmittedFrame> submittedFrames; bool requiredSubmitted; OutputFrameIdentity requiredIdentity; };`
  - `OutputDispatchReport OutputDispatcher::dispatchTickWithReport(...)`
- Consumes: existing endpoint assignments, `OutputBusFrame::identity`, and sink
  `submitAndFlush()`.

- [ ] **Step 1: Write failing output-dispatcher test**

Add a test that installs one PGM NDI-like sink and one slow Qt preview sink, then
dispatches `OutputDispatchLane::PgmCritical`. Expected behavior:

```cpp
QVERIFY(report.requiredSubmitted);
QCOMPARE(report.requiredIdentity.bus, OutputBusId::pgm());
QCOMPARE(pgmSink.submitCalls, 1);
QCOMPARE(previewSink.submitCalls, 0);
QVERIFY(!report.requiredIdentity.videoPlaceholder);
```

Run:

```sh
cmake --build build/c --target tst_outputdispatcher -j 8
./build/c/tests/unit/tst_outputdispatcher -o -,txt
```

Expected before implementation: compile failure for missing request/report types.

- [ ] **Step 2: Implement minimal dispatch request/report types**

Add the types to `outputdispatcher.h`. Preserve existing `dispatchTick()` by making
it call the new reporting method and return `report.stats`.

- [ ] **Step 3: Implement PGM-critical filtering**

In `dispatchTickWithReport`, when `request.lane == OutputDispatchLane::PgmCritical`,
only submit endpoints whose `assignment.sourceBus == OutputBusId::pgm()` and whose
kind matches `request.requiredKind` when a required kind is set. Do not submit Qt
preview endpoints in this lane.

- [ ] **Step 4: Record required submission evidence before preview work**

For every submitted endpoint, append `OutputSubmittedFrame`. Set
`requiredSubmitted` only when submit succeeds and the frame identity matches the
required bus/playhead/non-placeholder constraints.

- [ ] **Step 5: Verify output-dispatcher tests**

Run:

```sh
cmake --build build/c --target tst_outputdispatcher -j 8
./build/c/tests/unit/tst_outputdispatcher -o -,txt
```

Expected after implementation: all output-dispatcher tests pass.

---

### Task 2: OutputRuntime PGM-First Immediate Dispatch

**Files:**
- Modify: `playback/output/outputruntime.h`
- Modify: `playback/output/outputruntime.cpp`
- Test: `tests/unit/tst_outputruntime.cpp`

**Interfaces:**
- Consumes Task 1 `OutputDispatchRequest` and `OutputDispatchReport`.
- Produces:
  - `OutputDispatchReport OutputRuntime::dispatchImmediateWithReport(const OutputDispatchRequest& request);`
  - existing `dispatchImmediate()` remains as an all-lane compatibility wrapper.

- [ ] **Step 1: Write failing OutputRuntime test**

Add a test where `dispatchImmediateWithReport(PgmCritical)` returns before a slow
preview sink is submitted and reports the PGM identity.

Expected assertions:

```cpp
QVERIFY(report.requiredSubmitted);
QCOMPARE(report.requiredIdentity.bus, OutputBusId::pgm());
QCOMPARE(pgmSink.submitCalls, 1);
QCOMPARE(previewSink.submitCalls, 0);
```

Run:

```sh
cmake --build build/c --target tst_outputruntime -j 8
./build/c/tests/unit/tst_outputruntime -o -,txt
```

Expected before implementation: compile failure for missing runtime method.

- [ ] **Step 2: Implement runtime wrapper**

Mirror the locking and reconfiguration safety of `dispatchImmediate()`, but call
`m_dispatcher.dispatchTickWithReport(current.cache, current.state,
OutputDispatchFlushMode::PausedImmediate, request)`.

- [ ] **Step 3: Keep compatibility path unchanged**

Make `dispatchImmediate()` call `dispatchImmediateWithReport(OutputDispatchRequest{})`
and return `.stats`, preserving all existing callers.

- [ ] **Step 4: Verify runtime tests**

Run:

```sh
cmake --build build/c --target tst_outputruntime -j 8
./build/c/tests/unit/tst_outputruntime -o -,txt
```

Expected after implementation: all output-runtime tests pass.

---

### Task 3: PlaybackWorker Operator Seek Transaction And Wake

**Files:**
- Modify: `playback/playbackworker.h`
- Modify: `playback/playbackworker.cpp`
- Test: `tests/unit/tst_playbackworker.cpp`

**Interfaces:**
- Consumes Task 2 `dispatchImmediateWithReport()`.
- Produces:
  - `struct OperatorSeekResult { bool completed; bool submittedPgm; bool timedOut; qint64 targetMs; uint64_t generation; OutputFrameIdentity pgmIdentity; qint64 elapsedNs; QString message; };`
  - `OperatorSeekResult PlaybackWorker::seekToAndWaitForPgm(qint64 timestampMs, int directionHint, int timeoutMs);`
  - internal paused wake condition used by `seekTo()` and transaction seeks.

- [ ] **Step 1: Write failing transaction tests**

Add tests for:

- cache-hit paused seek completes inline and returns a PGM identity;
- cache-miss paused seek does not report completed until a later commit/PGM dispatch;
- a newer seek supersedes an older waiter;
- paused worker wait is woken by `seekTo()` instead of relying on a 10 ms sleep.

Run:

```sh
cmake --build build/c --target tst_playbackworker -j 8
./build/c/tests/unit/tst_playbackworker -o -,txt
```

Expected before implementation: compile failure for missing transaction API.

- [ ] **Step 2: Factor seek request creation**

Extract the common generation/target setup from `seekTo()` into a private helper
that returns `{clampedTarget, moveDir, seekGeneration, committedFromPublishedCache}`.
Keep existing `seekTo()` behavior for non-transaction callers.

- [ ] **Step 3: Add paused worker wake**

Add a `QWaitCondition m_workerWake` guarded by `m_mutex`. After setting
`m_seekTargetMs`, call `m_workerWake.wakeAll()`. Replace the paused `msleep(10)`
idle path with a timed wait on `m_workerWake`, rechecking `m_seekTargetMs` before
sleeping.

- [ ] **Step 4: Add PGM dispatch helper**

Add a private helper:

```cpp
OutputDispatchReport PlaybackWorker::dispatchPgmAfterSeekCommit(qint64 targetMs);
```

It builds an `OutputDispatchRequest` for `OutputDispatchLane::PgmCritical`,
`OutputBusId::pgm()`, non-placeholder frame, and the target playhead, then calls
`OutputRuntime::dispatchImmediateWithReport()`.

- [ ] **Step 5: Add waitable transaction**

`seekToAndWaitForPgm()` should:

1. start a timer;
2. enqueue the seek using the same helper as `seekTo()`;
3. for cache-hit coverage, dispatch PGM immediately and return the report;
4. for cache-miss coverage, wait on a transaction condition until the matching
   generation records PGM evidence or timeout/supersession occurs;
5. return `OperatorSeekResult` with exact identity and elapsed time.

- [ ] **Step 6: Fulfill transactions after reposition commit**

After `repositionTo()` commits a generation and publishes the cache, run the PGM
dispatch helper before preview/all-output refresh. Store the resulting identity in
the matching transaction state and wake waiters.

- [ ] **Step 7: Schedule preview after PGM**

After command-owned PGM dispatch evidence is recorded, schedule or run the existing
all-output refresh for previews and non-critical outputs. This must not be part of
transaction completion.

- [ ] **Step 8: Verify playback-worker tests**

Run:

```sh
cmake --build build/c --target tst_playbackworker -j 8
./build/c/tests/unit/tst_playbackworker -o -,txt
```

Expected after implementation: all playback-worker tests pass.

---

### Task 4: WebSocket And External Control Transaction Completion

**Files:**
- Modify: `uimanager.h`
- Modify: `uimanager.cpp`
- Modify: `websocket/uimanagercontroladapter.cpp`
- Test: `tests/unit/tst_controlwebsocketserver.cpp` or a focused adapter test if
  present.

**Interfaces:**
- Consumes Task 3 `PlaybackWorker::seekToAndWaitForPgm()`.
- Produces UI manager methods:
  - `OperatorSeekResult UIManager::seekPlaybackAndWaitForPgm(qint64 ms, int timeoutMs);`
  - `OperatorSeekResult UIManager::jogExternalAndWaitForPgm(int delta, int timeoutMs);`

- [ ] **Step 1: Write failing control-path test**

Add or extend a test so `transport.seek` and `transport.stepFrame` use the waitable
path and return failure when the transaction times out.

Expected before implementation: compile or assertion failure because commands still
return immediate success.

- [ ] **Step 2: Add UI manager waitable wrappers**

Keep existing QML methods intact. Add wrappers for WebSocket/test/external use that
perform the same transport updates but call `seekToAndWaitForPgm()` with a bounded
timeout.

- [ ] **Step 3: Update WebSocket adapter**

For `transport.seek`, `transport.stepFrame`, and `action.jog`, call the waitable UI
manager wrappers. Return `CommandResult::failure("timeout", ...)` when the result
does not complete. Keep ordinary play/pause/speed commands unchanged.

- [ ] **Step 4: Add command telemetry**

When a transaction completes or times out, log command name, target, generation,
PGM identity, elapsed ns, and timeout status under the existing latency trace flag.

- [ ] **Step 5: Verify control tests**

Run the focused control test target and the playback-worker target:

```sh
cmake --build build/c --target tst_controlwebsocketserver tst_playbackworker -j 8
./build/c/tests/unit/tst_controlwebsocketserver -o -,txt
./build/c/tests/unit/tst_playbackworker -o -,txt
```

Expected after implementation: all focused tests pass.

---

### Task 5: E2E Driver And Runbook Update

**Files:**
- Modify: `tests/e2e/macos_app_driver.py`
- Modify: `docs/frame-accurate-scrub-testing.md`
- Test: `tests/e2e/test_macos_app_driver_static.py`

**Interfaces:**
- Consumes WebSocket command ACKs that now mean PGM transaction completion.
- Produces clearer failure diagnostics for command ACK time vs PGM marker time.

- [ ] **Step 1: Write failing static-driver assertion**

Extend `test_macos_app_driver_static.py` to require that strict latency output logs
command ACK timing, PGM marker timing, and whether ACK happened after PGM transaction
completion.

- [ ] **Step 2: Update latency reporting**

Keep `run_command_with_ndi_latency()` measuring from before the command to marker
arrival. Add fields for command ACK elapsed and assert that ACK no longer completes
before the app has performed a transaction wait.

- [ ] **Step 3: Document PGM-first oracle**

Update `docs/frame-accurate-scrub-testing.md` to say strict mode measures PGM output
as the authority; OS screenshots are preview evidence only.

- [ ] **Step 4: Verify static tests**

Run:

```sh
python3 tests/e2e/test_macos_app_driver_static.py tests/e2e/macos_app_driver.py
```

Expected after implementation: PASS.

---

### Task 6: Integrated Verification

**Files:**
- No new source files expected; this task verifies the branch.

**Interfaces:**
- Consumes all earlier tasks.
- Produces the evidence needed before any iOS install is considered.

- [ ] **Step 1: Build focused targets**

Run:

```sh
cmake --build build/c --target \
  tst_outputdispatcher tst_outputruntime tst_playbackworker \
  tst_asyncgpureadbacksink OpenLiveReplay -j 8
```

Expected: build exits 0.

- [ ] **Step 2: Run focused unit tests**

Run:

```sh
./build/c/tests/unit/tst_outputdispatcher -o -,txt
./build/c/tests/unit/tst_outputruntime -o -,txt
./build/c/tests/unit/tst_playbackworker -o -,txt
./build/c/tests/unit/tst_asyncgpureadbacksink -o -,txt
```

Expected: all tests report 0 failures.

- [ ] **Step 3: Run macOS strict app E2E**

Run:

```sh
ctest --test-dir build/c -R e2e_macos_app_visual --output-on-failure
```

Expected: `APP_E2E_PASS` with all expected NDI latency samples and no sample above
25 ms.

- [ ] **Step 4: Inspect latency distribution**

From the E2E output and app log, record:

- max command-to-PGM marker latency;
- max command ACK elapsed;
- max GPU PGM render time;
- max GPU readback/NDI submit time;
- whether any preview update lagged PGM.

Expected: no hard-gate miss; if under-15 ms is not reached, the log identifies the
dominant remaining class before further optimization.

- [ ] **Step 5: Decide iOS readiness**

Only after the macOS strict app E2E passes, build/install iOS and run the local SRT
oracle from `docs/frame-accurate-scrub-testing.md`.
