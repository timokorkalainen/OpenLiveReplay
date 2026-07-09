# Async PGM Command Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `waitForPgm:true` operator commands non-blocking: an immediate
`accepted` ack plus one correlated `command.completed` event once the program output
has (or has definitively not) accepted the target frame.

**Architecture:** The playback worker emits a queued `operatorSeekCompleted` signal
from its single transaction-resolution point; `UIManager` relays it with a
per-worker epoch (the worker is recreated per session); a standalone
`PendingCommandRegistry` in the websocket layer correlates `(epoch, generation)` to
`(clientId, commandId)` with one-shot resolution, deadline, and supersession; the
server delivers the completion event to the originating socket only. The blocking
`seekPlaybackAndWaitForPgm` call disappears from the WebSocket adapter (the C++ API
stays for tests).

**Tech Stack:** C++17, Qt 6 (QObject signals, explicit `Qt::QueuedConnection`,
QWebSocket), QtTest, Python E2E drivers.

**Spec:** `docs/superpowers/specs/2026-07-09-async-pgm-command-completion-design.md`
(every task cites its spec section; deviations require updating the spec in the same
commit).

## Global Constraints

- Worktree: `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/ios-frame-residency`,
  branch `gpu/ios-frame-residency`.
- Build: `cmake -S . -B build/gpuon -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON`
  then `ninja -C build/gpuon <target>`. (A `build/gpuon` dir configured this way
  already exists.) Also keep the GPU-off `-Werror` config green: `build/gpuoff`.
- Tests: run the FULL unit label after each task
  (`QT_QPA_PLATFORM=offscreen ctest --test-dir build/gpuon -L unit --output-on-failure`)
  — worker/adapter changes affect siblings.
- **Commit gate:** the user has NOT granted blanket commit permission for this
  working tree. Before the first commit of this plan's execution, confirm the user
  has authorized committing. Never `git push --no-verify`.
- All new `operatorSeekCompleted` connections MUST be explicit `Qt::QueuedConnection`
  with the result passed **by value** (spec §2 connection discipline).
- Every `emit` added inside a mutex scope is safe only because of that queued
  discipline — do not add any `Qt::DirectConnection`/`AutoConnection` consumer.
- Format changed lines only:
  `python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main --extensions cpp,h,hpp,mm,c`.
- Public repo: comments/messages document present design only.
- Commits end with `Co-Authored-By: Claude <noreply@anthropic.com>`.

## File Structure

- `playback/playbackworker.h/.cpp` — completion signal, superseded emission,
  `abandonOperatorSeekTransaction`, `seekToWithPgmNotify` (Tasks 1–2).
- `websocket/pendingcommandregistry.h/.cpp` — NEW standalone correlation component
  (Task 3).
- `websocket/controlprotocol.cpp` — `waitForPgm`-requires-id validation (Task 4).
- `websocket/controlapiadapter.h` — `CommandResult::accepted` factory + two defaulted
  interface hooks (Task 4/6).
- `uimanager.h/.cpp` — worker epoch, relay signal, wiring, async enqueue methods
  (Task 5).
- `websocket/uimanagercontroladapter.h/.cpp` — registry integration, non-blocking
  `executeCommand` (Task 6).
- `websocket/controlwebsocketserver.h/.cpp` — serial clientId, `_commandId`
  injection, per-socket completion delivery, disconnect purge (Task 7).
- `tests/unit/tst_playbackworker.cpp`, NEW `tests/unit/tst_pendingcommandregistry.cpp`,
  `tests/unit/tst_controlprotocol.cpp`, `tests/unit/tst_controlwebsocketserver.cpp` —
  unit coverage.
- `tests/e2e/macos_app_driver.py`, `tests/e2e/ios_marker_srt_oracle.py`,
  `tests/e2e/test_control_adapter_wait_static.py` — migrated consumers (Task 8).

---

### Task 1: Worker completion signal, superseded emission, abandon API

**Spec:** §2 (single emission point, worker-side supersession, abandonment API).

**Files:**
- Modify: `playback/playbackworker.h` (class `PlaybackWorker`, `Q_OBJECT` at line ~78;
  public methods near `seekToAndWaitForPgm` at line ~157; private decls near
  `completeOperatorSeekTransaction` at line ~298)
- Modify: `playback/playbackworker.cpp` (`requestSeekTo` at :192, transaction-slot
  registration at :230-235; `completeOperatorSeekTransaction` at :493-510;
  `seekToAndWaitForPgm` timeout branch at :340-351)
- Test: `tests/unit/tst_playbackworker.cpp`

**Interfaces:**
- Consumes: existing `OperatorSeekResult`, `OperatorSeekCompletionState`,
  `requestSeekTo(qint64, int, bool)`.
- Produces (later tasks rely on these exact names):
  - signal `void operatorSeekCompleted(quint64 generation, PlaybackWorker::OperatorSeekResult result);`
  - `void abandonOperatorSeekTransaction(quint64 generation);` (public, thread-safe)
  - superseded results carry `message == QStringLiteral("superseded")`,
    `completed == false`, `submittedPgm == false`.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/tst_playbackworker.cpp`;
  register the three slots in the class's `private slots:` list next to
  `operatorSeekTransactionAbandonedOnTimeout`)

```cpp
void TestPlaybackWorker::operatorSeekCompletionEmitsSignal() {
    // Harness identical to operatorSeekTransactionTimesOutWhenSeekGenerationUncommitted
    // (tst_playbackworker.cpp:850) up to the runtime/sink setup, except the cache DOES
    // cover the target so the blocking path completes inline.
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 88));
        worker.publishOutputCacheLocked();
    }

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    TestPgmSink pgmSink;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(60, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    QSignalSpy spy(&worker, &PlaybackWorker::operatorSeekCompleted);
    const PlaybackWorker::OperatorSeekResult result = worker.seekToAndWaitForPgm(1000, -1, 50);
    QVERIFY(result.completed);

    QTRY_COMPARE(spy.count(), 1); // queued delivery drains on the test event loop
    const auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toULongLong(), quint64(result.generation));
    const auto emitted = args.at(1).value<PlaybackWorker::OperatorSeekResult>();
    QVERIFY(emitted.submittedPgm);
    QCOMPARE(emitted.targetMs, qint64(1000));
    QCOMPARE(emitted.pgmIdentity.sampledPlayheadMs, result.pgmIdentity.sampledPlayheadMs);
}

void TestPlaybackWorker::generationBumpEmitsSupersededForWaitingTransaction() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    // Register a transaction the worker never resolves (uncovered target)...
    const PlaybackWorker::SeekRequestResult first = worker.requestSeekTo(5000, 1, true);
    QVERIFY(!first.committedFromPublishedCache);
    QVERIFY(worker.hasOperatorSeekTransaction(first.generation));

    QSignalSpy spy(&worker, &PlaybackWorker::operatorSeekCompleted);
    // ...then bump the generation WITHOUT registering a transaction (a QML/local seek).
    const PlaybackWorker::SeekRequestResult second = worker.requestSeekTo(9000, 1, false);
    QVERIFY(second.generation > first.generation);

    QTRY_COMPARE(spy.count(), 1);
    const auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toULongLong(), quint64(first.generation));
    const auto emitted = args.at(1).value<PlaybackWorker::OperatorSeekResult>();
    QVERIFY(!emitted.completed);
    QVERIFY(!emitted.submittedPgm);
    QCOMPARE(emitted.message, QStringLiteral("superseded"));
    QVERIFY(!worker.hasOperatorSeekTransaction(first.generation)); // slot cleared
}

void TestPlaybackWorker::abandonSuppressesLaterCompletion() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    const PlaybackWorker::SeekRequestResult seek = worker.requestSeekTo(5000, 1, true);
    QVERIFY(worker.hasOperatorSeekTransaction(seek.generation));

    worker.abandonOperatorSeekTransaction(seek.generation);
    QVERIFY(!worker.hasOperatorSeekTransaction(seek.generation));

    QSignalSpy spy(&worker, &PlaybackWorker::operatorSeekCompleted);
    OutputDispatchReport lateReport;
    lateReport.requiredSubmitted = true;
    worker.completeOperatorSeekTransaction(seek.generation, seek.clampedTargetMs, lateReport);
    QCoreApplication::processEvents();
    QCOMPARE(spy.count(), 0); // abandoned: no completion, no emission
}
```

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build/gpuon tst_playbackworker`
Expected: compile FAILS — `operatorSeekCompleted` signal and
`abandonOperatorSeekTransaction` do not exist.

- [ ] **Step 3: Implement**

`playback/playbackworker.h` — add in the class body:

```cpp
// In the public section, next to seekToAndWaitForPgm (~line 157):
    // Abandon a still-waiting operator transaction (thread-safe). Mirrors the
    // blocking wait loop's timeout behavior: the worker will not later complete
    // it or submit PGM for it. No-op if the generation does not match or the
    // transaction already completed.
    void abandonOperatorSeekTransaction(quint64 generation);

// New signals section (PlaybackWorker derives QThread, which is a QObject; add
// before the private section):
signals:
    // Emitted (queued consumers only — connect with explicit Qt::QueuedConnection,
    // result passed by value) whenever an operator transaction resolves:
    //   completed && submittedPgm            -> PGM accepted the committed target
    //   !submittedPgm, message == "PGM output was not submitted" -> dispatch failed
    //   !completed, message == "superseded"  -> a newer seek replaced it
    void operatorSeekCompleted(quint64 generation, PlaybackWorker::OperatorSeekResult result);
```

`playback/playbackworker.cpp`:

(a) In `requestSeekTo`, immediately BEFORE the generation bump at :229
(`result.generation = m_seekGeneration.fetch_add(...)`), snapshot and clear a
still-waiting transaction — every bump supersedes it, whether or not the new seek
registers one (spec §2):

```cpp
        // Any seek that advances the generation supersedes a still-waiting operator
        // transaction — including local QML scrubs, live-follow and playlist jumps,
        // which bump the generation without registering transactions. Snapshot the
        // orphaned command before overwriting so its completion can be reported.
        quint64 supersededGeneration = 0;
        qint64 supersededTargetMs = -1;
        if (m_operatorSeekCompletion.waiting && !m_operatorSeekCompletion.completed) {
            supersededGeneration = m_operatorSeekCompletion.generation;
            supersededTargetMs = m_operatorSeekCompletion.targetMs;
            m_operatorSeekCompletion.waiting = false;
        }
```

and at the END of the same `QMutexLocker` scope (right after
`m_workerWake.wakeAll();`):

```cpp
        if (supersededGeneration != 0) {
            OperatorSeekResult superseded;
            superseded.completed = false;
            superseded.submittedPgm = false;
            superseded.targetMs = supersededTargetMs;
            superseded.generation = supersededGeneration;
            superseded.message = QStringLiteral("superseded");
            // Queued consumers only; emitting under m_mutex posts an event and returns.
            emit operatorSeekCompleted(supersededGeneration, superseded);
        }
```

(b) In `completeOperatorSeekTransaction` (:493), after
`m_operatorSeekCondition.wakeAll();`, still inside the `QMutexLocker` scope:

```cpp
    OperatorSeekResult emitted;
    emitted.completed = report.requiredSubmitted;
    emitted.submittedPgm = report.requiredSubmitted;
    emitted.targetMs = targetMs;
    emitted.generation = generation;
    emitted.pgmIdentity = m_operatorSeekCompletion.pgmIdentity;
    emitted.message = m_operatorSeekCompletion.message;
    emit operatorSeekCompleted(generation, emitted);
```

(c) New method (place next to `hasOperatorSeekTransaction`, ~:503):

```cpp
void PlaybackWorker::abandonOperatorSeekTransaction(quint64 generation) {
    QMutexLocker locker(&m_mutex);
    if (m_operatorSeekCompletion.generation == generation && m_operatorSeekCompletion.waiting &&
        !m_operatorSeekCompletion.completed) {
        m_operatorSeekCompletion.waiting = false;
    }
}
```

(d) Simplify the blocking timeout branch in `seekToAndWaitForPgm` (:341-346) to use
the same state change (keep behavior identical — the existing inline clear is what
`abandonOperatorSeekTransaction` now encapsulates; the mutex is already held there,
so keep the inline field write, do NOT call the locking method from inside the lock).

- [ ] **Step 4: Run the tests**

Run: `ninja -C build/gpuon tst_playbackworker && QT_QPA_PLATFORM=offscreen ./build/gpuon/tests/unit/tst_playbackworker`
Expected: all tests pass, including the three new ones and the existing transaction
tests (`operatorSeekTransactionCompletesCoveredSeekWithPgmEvidence` etc. — the new
emissions must not break the blocking path).

- [ ] **Step 5: Full unit label**

Run: `QT_QPA_PLATFORM=offscreen ctest --test-dir build/gpuon -L unit --output-on-failure`
Expected: 100% pass.

- [ ] **Step 6: Commit** (after confirming the commit gate in Global Constraints)

```bash
git add playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_playbackworker.cpp
git commit -m "playback: emit operator seek completion; supersede on generation bump

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Worker non-blocking transactional seek (`seekToWithPgmNotify`)

**Spec:** §1 (non-blocking enqueue with the cache-hit clause).

**Files:**
- Modify: `playback/playbackworker.h` (public section, next to `seekToAndWaitForPgm`)
- Modify: `playback/playbackworker.cpp` (place next to `seekToAndWaitForPgm`, ~:309)
- Test: `tests/unit/tst_playbackworker.cpp`

**Interfaces:**
- Consumes: Task 1 signal; existing `requestSeekTo`, `dispatchPgmAfterSeekCommit`,
  `completeOperatorSeekTransaction`, `refreshPreviewAfterSeekCommit`.
- Produces: `quint64 seekToWithPgmNotify(qint64 timestampMs, int directionHint);`
  — returns the registered generation; completion arrives ONLY via
  `operatorSeekCompleted`.

- [ ] **Step 1: Write the failing tests**

```cpp
void TestPlaybackWorker::seekToWithPgmNotifyCompletesCacheHitInline() {
    // Same covered-cache harness as operatorSeekCompletionEmitsSignal (Task 1).
    // ... [build worker + published cache covering 1000ms + PGM NDI sink exactly
    //      as in operatorSeekCompletionEmitsSignal] ...

    QSignalSpy spy(&worker, &PlaybackWorker::operatorSeekCompleted);
    const quint64 generation = worker.seekToWithPgmNotify(1000, -1);
    QVERIFY(generation > 0);

    // The cache-hit clause must dispatch PGM inline (no worker thread running here).
    QCOMPARE(pgmSink.frames.size(), 1);
    QCOMPARE(pgmSink.frames.first().sampledPlayheadMs, qint64(1000));

    QTRY_COMPARE(spy.count(), 1);
    const auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toULongLong(), generation);
    QVERIFY(args.at(1).value<PlaybackWorker::OperatorSeekResult>().submittedPgm);
}

void TestPlaybackWorker::seekToWithPgmNotifyLeavesCacheMissWaiting() {
    // Same harness but the cache does NOT cover the target (insert frame at 1000,
    // seek to 5000). No wait, no PGM submit, transaction registered.
    // ... [harness] ...
    QSignalSpy spy(&worker, &PlaybackWorker::operatorSeekCompleted);
    const quint64 generation = worker.seekToWithPgmNotify(5000, 1);
    QVERIFY(worker.hasOperatorSeekTransaction(generation)); // still waiting — no block
    QCOMPARE(pgmSink.frames.size(), 0);
    QCoreApplication::processEvents();
    QCOMPARE(spy.count(), 0); // nothing resolved yet
}
```

(The second test is the structural non-blocking guard from the spec's Testing
contract: it proves the call returned while the transaction still waits.)

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build/gpuon tst_playbackworker`
Expected: compile FAILS — `seekToWithPgmNotify` does not exist.

- [ ] **Step 3: Implement** (`playback/playbackworker.cpp`, mirroring the pre-wait
  portion of `seekToAndWaitForPgm` at :309-322)

```cpp
quint64 PlaybackWorker::seekToWithPgmNotify(qint64 timestampMs, int directionHint) {
    const SeekRequestResult seek = requestSeekTo(timestampMs, directionHint, true);
    if (seek.committedFromPublishedCache) {
        // The worker never repositions for an inline-committed generation
        // (requestSeekTo cleared m_seekTargetMs), so the PGM dispatch and the
        // transaction completion are this caller's job — same as the blocking path.
        const OutputDispatchReport report = dispatchPgmAfterSeekCommit(seek.clampedTargetMs);
        completeOperatorSeekTransaction(seek.generation, seek.clampedTargetMs, report);
        refreshPreviewAfterSeekCommit();
    }
    return seek.generation;
}
```

Header declaration (next to `seekToAndWaitForPgm`):

```cpp
    // Non-blocking transactional seek: registers the operator transaction, performs
    // the inline cache-hit PGM dispatch when the target is already covered, and
    // returns the generation. Completion is reported via operatorSeekCompleted.
    quint64 seekToWithPgmNotify(qint64 timestampMs, int directionHint);
```

- [ ] **Step 4: Run tests + full unit label** (same commands as Task 1 Steps 4–5).
  Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_playbackworker.cpp
git commit -m "playback: add non-blocking transactional seek with inline cache-hit PGM

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: PendingCommandRegistry (new component + tests)

**Spec:** §3 (standalone, testable registry; epoch keying; one-shot; deadline).

**Files:**
- Create: `websocket/pendingcommandregistry.h`, `websocket/pendingcommandregistry.cpp`
- Modify: `CMakeLists.txt` (app sources, websocket block at :198-202 — add both files)
- Modify: `tests/CMakeLists.txt` (olr_test_core sources, :111-113 — add the .cpp)
- Create: `tests/unit/tst_pendingcommandregistry.cpp`
- Modify: `tests/unit/CMakeLists.txt` (register next to `tst_controlwebsocketserver`
  at :36): `olr_add_unit_test(tst_pendingcommandregistry olr_test_core)`

**Interfaces:**
- Consumes: nothing project-specific (QString/QJsonObject only — no playback types,
  so `olr_test_core` needs no new deps).
- Produces (Tasks 6–7 rely on these exact names):

```cpp
class PendingCommandRegistry : public QObject {
    Q_OBJECT
public:
    explicit PendingCommandRegistry(QObject* parent = nullptr);
    void setDeadlineMs(int ms);                       // default 1000
    void registerPending(quint64 epoch, quint64 generation, const QString& clientId,
                         const QString& commandId);
    void resolve(quint64 epoch, quint64 generation, QJsonObject completion);
    void noteWorkerEpoch(quint64 epoch);              // older-epoch entries -> superseded
    void dropClient(const QString& clientId);         // disconnect purge
    void checkDeadlines(qint64 nowMs);                // public for tests; timer calls it
signals:
    void commandCompleted(const QString& clientId, const QString& commandId,
                          const QJsonObject& completion);
    void pendingExpired(quint64 epoch, quint64 generation); // consumer abandons the txn
};
```

Completion JSON contract: `resolve()` delivers the caller's object after inserting
`latencyMs` (ms since `registerPending`). The deadline path emits
`{done:false, reason:"timeout", latencyMs}` and `pendingExpired`. Epoch/`dropClient`
supersession emits `{done:false, reason:"superseded"}` / drops silently
(disconnected client has no socket to notify).

- [ ] **Step 1: Write the failing tests** (`tests/unit/tst_pendingcommandregistry.cpp`)

```cpp
#include <QtTest>
#include <QJsonObject>
#include <QSignalSpy>

#include "websocket/pendingcommandregistry.h"

class TestPendingCommandRegistry : public QObject {
    Q_OBJECT
private slots:
    void resolveDeliversOnceWithLatency() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.registerPending(1, 42, QStringLiteral("client-7"), QStringLiteral("cmd-9"));

        QJsonObject completion{{QStringLiteral("done"), true},
                               {QStringLiteral("pgmPts"), 5000}};
        registry.resolve(1, 42, completion);
        QCOMPARE(done.count(), 1);
        auto args = done.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("client-7"));
        QCOMPARE(args.at(1).toString(), QStringLiteral("cmd-9"));
        const QJsonObject delivered = args.at(2).toJsonObject();
        QVERIFY(delivered.value(QStringLiteral("done")).toBool());
        QVERIFY(delivered.contains(QStringLiteral("latencyMs")));

        registry.resolve(1, 42, completion); // one-shot: second resolve is dropped
        QCOMPARE(done.count(), 0);
    }

    void unmatchedResolveIsDropped() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.resolve(1, 999, QJsonObject{{QStringLiteral("done"), true}});
        QCOMPARE(done.count(), 0);
    }

    void deadlineResolvesTimeoutAndExpires() {
        PendingCommandRegistry registry;
        registry.setDeadlineMs(50);
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        QSignalSpy expired(&registry, &PendingCommandRegistry::pendingExpired);
        registry.registerPending(1, 42, QStringLiteral("c"), QStringLiteral("i"));

        registry.checkDeadlines(registry.nowMsForTest() + 49); // not yet
        QCOMPARE(done.count(), 0);
        registry.checkDeadlines(registry.nowMsForTest() + 51);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.takeFirst().at(2).toJsonObject().value(QStringLiteral("reason")).toString(),
                 QStringLiteral("timeout"));
        QCOMPARE(expired.count(), 1);
        QCOMPARE(expired.takeFirst().at(1).toULongLong(), quint64(42));
    }

    void epochChangeSupersedesOlderEntries() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.registerPending(1, 42, QStringLiteral("c"), QStringLiteral("i"));
        registry.noteWorkerEpoch(2);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.takeFirst().at(2).toJsonObject().value(QStringLiteral("reason")).toString(),
                 QStringLiteral("superseded"));
        registry.resolve(1, 42, QJsonObject{{QStringLiteral("done"), true}}); // stale
        QCOMPARE(done.count(), 0);
    }

    void dropClientRemovesSilently() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.registerPending(1, 42, QStringLiteral("gone"), QStringLiteral("i"));
        registry.dropClient(QStringLiteral("gone"));
        QCOMPARE(done.count(), 0);
        registry.resolve(1, 42, QJsonObject{{QStringLiteral("done"), true}});
        QCOMPARE(done.count(), 0); // entry is gone
    }
};

QTEST_GUILESS_MAIN(TestPendingCommandRegistry)
#include "tst_pendingcommandregistry.moc"
```

- [ ] **Step 2: Run to verify failure** — configure once (`cmake` re-run picks up the
  CMake edits), then `ninja -C build/gpuon tst_pendingcommandregistry`.
  Expected: compile FAILS — header does not exist.

- [ ] **Step 3: Implement**

`websocket/pendingcommandregistry.h`:

```cpp
#ifndef PENDINGCOMMANDREGISTRY_H
#define PENDINGCOMMANDREGISTRY_H

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>

// Correlates accepted transactional operator commands (keyed by playback-worker
// epoch + seek generation) with the client/command that issued them, and delivers
// exactly one completion per entry: worker resolution, supersession, client
// departure, or deadline — whichever comes first. Protocol-layer types only
// (no playback dependencies) so it unit-tests in isolation.
class PendingCommandRegistry : public QObject {
    Q_OBJECT
public:
    explicit PendingCommandRegistry(QObject* parent = nullptr);

    void setDeadlineMs(int ms);
    void registerPending(quint64 epoch, quint64 generation, const QString& clientId,
                         const QString& commandId);
    void resolve(quint64 epoch, quint64 generation, QJsonObject completion);
    void noteWorkerEpoch(quint64 epoch);
    void dropClient(const QString& clientId);

    // Deadline scan against a caller-supplied monotonic timestamp; the internal
    // timer calls this with nowMsForTest(). Public so tests drive it directly.
    void checkDeadlines(qint64 nowMs);
    qint64 nowMsForTest() const { return m_clock.elapsed(); }

signals:
    void commandCompleted(const QString& clientId, const QString& commandId,
                          const QJsonObject& completion);
    void pendingExpired(quint64 epoch, quint64 generation);

private:
    struct Entry {
        QString clientId;
        QString commandId;
        qint64 registeredAtMs = 0;
    };
    void deliverLocked(const QPair<quint64, quint64>& key, const Entry& entry,
                       QJsonObject completion, qint64 nowMs);
    void updateTimer();

    QHash<QPair<quint64, quint64>, Entry> m_entries;
    QElapsedTimer m_clock;
    QTimer m_timer;
    int m_deadlineMs = 1000;
    quint64 m_currentEpoch = 0;
};

#endif
```

`websocket/pendingcommandregistry.cpp`:

```cpp
#include "pendingcommandregistry.h"

PendingCommandRegistry::PendingCommandRegistry(QObject* parent) : QObject(parent) {
    m_clock.start();
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this]() { checkDeadlines(m_clock.elapsed()); });
}

void PendingCommandRegistry::setDeadlineMs(int ms) {
    m_deadlineMs = ms > 0 ? ms : 1000;
}

void PendingCommandRegistry::registerPending(quint64 epoch, quint64 generation,
                                             const QString& clientId,
                                             const QString& commandId) {
    if (epoch > m_currentEpoch) m_currentEpoch = epoch;
    Entry entry;
    entry.clientId = clientId;
    entry.commandId = commandId;
    entry.registeredAtMs = m_clock.elapsed();
    m_entries.insert({epoch, generation}, entry);
    updateTimer();
}

void PendingCommandRegistry::resolve(quint64 epoch, quint64 generation,
                                     QJsonObject completion) {
    const auto key = QPair<quint64, quint64>{epoch, generation};
    const auto it = m_entries.constFind(key);
    if (it == m_entries.cend()) return; // one-shot: already resolved, or never ours
    const Entry entry = it.value();
    m_entries.erase(it);
    deliverLocked(key, entry, std::move(completion), m_clock.elapsed());
    updateTimer();
}

void PendingCommandRegistry::noteWorkerEpoch(quint64 epoch) {
    if (epoch <= m_currentEpoch) return;
    m_currentEpoch = epoch;
    const auto entries = m_entries;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (it.key().first >= epoch) continue;
        m_entries.remove(it.key());
        QJsonObject completion{{QStringLiteral("done"), false},
                               {QStringLiteral("reason"), QStringLiteral("superseded")}};
        deliverLocked(it.key(), it.value(), std::move(completion), m_clock.elapsed());
    }
    updateTimer();
}

void PendingCommandRegistry::dropClient(const QString& clientId) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        it = it.value().clientId == clientId ? m_entries.erase(it) : std::next(it);
    }
    updateTimer();
}

void PendingCommandRegistry::checkDeadlines(qint64 nowMs) {
    const auto entries = m_entries;
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (nowMs - it.value().registeredAtMs < m_deadlineMs) continue;
        m_entries.remove(it.key());
        emit pendingExpired(it.key().first, it.key().second);
        QJsonObject completion{{QStringLiteral("done"), false},
                               {QStringLiteral("reason"), QStringLiteral("timeout")}};
        deliverLocked(it.key(), it.value(), std::move(completion), nowMs);
    }
    updateTimer();
}

void PendingCommandRegistry::deliverLocked(const QPair<quint64, quint64>& key,
                                           const Entry& entry, QJsonObject completion,
                                           qint64 nowMs) {
    Q_UNUSED(key);
    completion.insert(QStringLiteral("latencyMs"),
                      static_cast<double>(nowMs - entry.registeredAtMs));
    emit commandCompleted(entry.clientId, entry.commandId, completion);
}

void PendingCommandRegistry::updateTimer() {
    if (m_entries.isEmpty()) {
        m_timer.stop();
    } else if (!m_timer.isActive()) {
        m_timer.start();
    }
}
```

CMake — `CMakeLists.txt` websocket block (after :202) and `tests/CMakeLists.txt`
olr_test_core list (after :113):

```cmake
        websocket/pendingcommandregistry.h websocket/pendingcommandregistry.cpp
```
```cmake
    "${CMAKE_SOURCE_DIR}/websocket/pendingcommandregistry.cpp"
```

- [ ] **Step 4: Run tests + full unit label.** Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add websocket/pendingcommandregistry.h websocket/pendingcommandregistry.cpp \
        CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
        tests/unit/tst_pendingcommandregistry.cpp
git commit -m "websocket: add pending command registry for async PGM completion

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Protocol — `waitForPgm` requires an id; `CommandResult::accepted`

**Spec:** Protocol section (no-id rejection; accepted-ack keeps `ok:true`).

**Files:**
- Modify: `websocket/controlprotocol.cpp` (`validateCommand` — the shared
  `waitForPgm` boolean check is at :61-63)
- Modify: `websocket/controlapiadapter.h` (`CommandResult`, :101-118)
- Test: `tests/unit/tst_controlprotocol.cpp`

**Interfaces:**
- Produces:
  - validation: seek/step/jog with `args.waitForPgm == true` and an empty command
    `id` → invalid, code `bad_message`.
  - `static CommandResult CommandResult::accepted(quint64 workerEpoch, quint64 generation);`
    → `ok=true`, `details = {status:"accepted", generation:"<decimal string>",
    workerEpoch:"<decimal string>"}` (strings, matching the existing
    `QString::number` convention for 64-bit values in `pgmTransactionDetails`).

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/tst_controlprotocol.cpp`,
  following its existing `validateCommand` test style)

```cpp
void TestControlProtocol::waitForPgmRequiresCommandId() {
    ControlCommandMessage command;
    command.type = QStringLiteral("command");
    command.name = QStringLiteral("transport.seek");
    command.args = QJsonObject{{QStringLiteral("positionMs"), 1000},
                               {QStringLiteral("waitForPgm"), true}};
    command.id = QString(); // no id -> completion could never be correlated
    const auto validation = ControlProtocol::validateCommand(command);
    QVERIFY(!validation.ok);

    command.id = QStringLiteral("cmd-1");
    QVERIFY(ControlProtocol::validateCommand(command).ok);

    // Without waitForPgm an empty id stays legal (fire-and-forget).
    command.id = QString();
    command.args.remove(QStringLiteral("waitForPgm"));
    QVERIFY(ControlProtocol::validateCommand(command).ok);
}

void TestControlProtocol::acceptedResultShape() {
    const CommandResult result = CommandResult::accepted(3, 42);
    QVERIFY(result.ok);
    QCOMPARE(result.details.value(QStringLiteral("status")).toString(),
             QStringLiteral("accepted"));
    QCOMPARE(result.details.value(QStringLiteral("generation")).toString(),
             QStringLiteral("42"));
    QCOMPARE(result.details.value(QStringLiteral("workerEpoch")).toString(),
             QStringLiteral("3"));
}
```

- [ ] **Step 2: Run to verify failure** —
  `ninja -C build/gpuon tst_controlprotocol && QT_QPA_PLATFORM=offscreen ./build/gpuon/tests/unit/tst_controlprotocol`
  Expected: FAIL (no id-requirement, no `accepted` factory).

- [ ] **Step 3: Implement**

`controlprotocol.cpp` — in `validateCommand`, immediately after the existing shared
`waitForPgm` boolean-type check (:61-63):

```cpp
    if (args.value(QStringLiteral("waitForPgm")).toBool(false) && command.id.trimmed().isEmpty() &&
        (name == QStringLiteral("transport.seek") || name == QStringLiteral("transport.stepFrame") ||
         name == QStringLiteral("action.jog"))) {
        return invalid(name +
                       QStringLiteral(" with waitForPgm requires a command id for completion "
                                      "correlation"));
    }
```

(If the :61 check lives in a helper that receives only `args`, add this clause in
`validateCommand`'s body where both `command.id` and `command.name` are in scope,
directly before the per-name dispatch.)

`controlapiadapter.h` — inside `struct CommandResult`, after `success(QJsonObject)`:

```cpp
    static CommandResult accepted(quint64 workerEpoch, quint64 generation) {
        CommandResult result;
        result.details = QJsonObject{
            {QStringLiteral("status"), QStringLiteral("accepted")},
            {QStringLiteral("generation"), QString::number(generation)},
            {QStringLiteral("workerEpoch"), QString::number(workerEpoch)}};
        return result;
    }
```

- [ ] **Step 4: Run tests + full unit label.** Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add websocket/controlprotocol.cpp websocket/controlapiadapter.h tests/unit/tst_controlprotocol.cpp
git commit -m "control: validate waitForPgm correlation id; add accepted result shape

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: UIManager — worker epoch, relay signal, async enqueue

**Spec:** §1 (async enqueue), §3 (epoch keying, wiring/ownership).

**Files:**
- Modify: `uimanager.h` (signals section at :363; public methods near
  `seekPlaybackAndWaitForPgm` at :331-332; private members + helper)
- Modify: `uimanager.cpp` (worker construction sites :1880 and :1907; new methods next
  to `seekPlaybackAndWaitForPgm` at :1958 and `jogStep` at :792)

**Interfaces:**
- Consumes: Task 1 signal + abandon API, Task 2 `seekToWithPgmNotify`.
- Produces (Task 6 relies on these exact names):

```cpp
    struct AsyncSeekTicket {
        quint64 workerEpoch = 0;
        quint64 generation = 0;
        bool accepted = false;
        QString message; // failure reason when !accepted
    };
    AsyncSeekTicket seekPlaybackAsyncPgm(int64_t ms);
    AsyncSeekTicket jogExternalAsyncPgm(int delta);
    void abandonOperatorSeek(quint64 workerEpoch, quint64 generation);
signals:
    void operatorSeekCompleted(quint64 workerEpoch, quint64 generation,
                               PlaybackWorker::OperatorSeekResult result);
    void playbackWorkerEpochChanged(quint64 workerEpoch);
```

**No direct unit test** — UIManager is not compiled into any test library; this task
is covered by the worker tests (Tasks 1–2), the adapter/server tests (Tasks 6–7),
the static wiring guard (Task 8), and the E2E. Keep the implementation a thin
composition of already-tested pieces.

- [ ] **Step 1: Implement the epoch + relay wiring**

`uimanager.h` — private members and helper:

```cpp
    quint64 m_playbackWorkerEpoch = 0;
    void wirePlaybackWorkerCompletion();
```

`uimanager.cpp` — the helper (place near `restartPlaybackWorker`):

```cpp
void UIManager::wirePlaybackWorkerCompletion() {
    // The worker is recreated per playback session; its generation counter restarts,
    // so completions are keyed by (epoch, generation). The queued relay dies with
    // each worker and is re-made here; the control adapter connects once to this
    // stable UIManager signal instead of the transient worker.
    ++m_playbackWorkerEpoch;
    emit playbackWorkerEpochChanged(m_playbackWorkerEpoch);
    if (!m_playbackWorker) return;
    const quint64 epoch = m_playbackWorkerEpoch;
    connect(
        m_playbackWorker, &PlaybackWorker::operatorSeekCompleted, this,
        [this, epoch](quint64 generation, const PlaybackWorker::OperatorSeekResult& result) {
            emit operatorSeekCompleted(epoch, generation, result);
        },
        Qt::QueuedConnection);
}
```

Call `wirePlaybackWorkerCompletion();` immediately after BOTH construction sites'
setup blocks — after `m_playbackWorker->setExternalOutputTargets(...)` at :1884
(startRecording path) and at :1911 (restartPlaybackWorker path).

- [ ] **Step 2: Implement the async enqueue methods** (next to their blocking twins,
  mirroring their transport-side preambles exactly)

```cpp
UIManager::AsyncSeekTicket UIManager::seekPlaybackAsyncPgm(int64_t ms) {
    AsyncSeekTicket ticket;
    // Same operator-override preamble as seekPlaybackAndWaitForPgm (uimanager.cpp:1958).
    stopPlaylistPlayout();
    setFollowLive(false);
    m_scrubCoalesceTimer.stop();
    m_seekCoalescer.reset();
    if (!m_transport) {
        ticket.message = QStringLiteral("transport unavailable");
        return ticket;
    }
    const int directionHint = ms < m_transport->currentPos() ? -1 : 1;
    m_transport->seek(ms);
    if (!m_playbackWorker) {
        ticket.message = QStringLiteral("playback worker unavailable");
        return ticket;
    }
    ticket.workerEpoch = m_playbackWorkerEpoch;
    ticket.generation = m_playbackWorker->seekToWithPgmNotify(qMax<int64_t>(0, ms), directionHint);
    ticket.accepted = true;
    return ticket;
}

UIManager::AsyncSeekTicket UIManager::jogExternalAsyncPgm(int delta) {
    AsyncSeekTicket ticket;
    if (!m_transport || delta == 0) {
        ticket.message = delta == 0 ? QStringLiteral("no-op")
                                    : QStringLiteral("transport unavailable");
        return ticket;
    }
    // Same jog preamble as jogStep (uimanager.cpp:792): pause, leave live-follow,
    // step the transport, clamp forward jogs to the live edge.
    m_transport->setPlaying(false);
    cancelFollowLive();
    m_transport->step(delta);
    if (delta > 0) {
        const int64_t liveEdge = recordedDurationMs();
        if (m_transport->currentPos() > liveEdge) m_transport->seek(liveEdge);
    }
    if (!m_playbackWorker) {
        ticket.message = QStringLiteral("playback worker unavailable");
        return ticket;
    }
    ticket.workerEpoch = m_playbackWorkerEpoch;
    ticket.generation = m_playbackWorker->seekToWithPgmNotify(m_transport->currentPos(), delta);
    ticket.accepted = true;
    return ticket;
}

void UIManager::abandonOperatorSeek(quint64 workerEpoch, quint64 generation) {
    if (workerEpoch != m_playbackWorkerEpoch || !m_playbackWorker) return;
    m_playbackWorker->abandonOperatorSeekTransaction(generation);
}
```

- [ ] **Step 3: Build both configs**

Run: `ninja -C build/gpuon && ninja -C build/gpuoff`
Expected: both compile (`-Werror` clean).

- [ ] **Step 4: Full unit label.** Expected: PASS (no behavior change for existing
  paths — the new methods are not yet called).

- [ ] **Step 5: Commit**

```bash
git add uimanager.h uimanager.cpp
git commit -m "ui: relay operator seek completion with per-worker epoch

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Adapter — non-blocking `executeCommand` + registry integration

**Spec:** §3 (registry ownership), Scope decision (blocking path removed), Semantics
(outcome mapping).

**Files:**
- Modify: `websocket/uimanagercontroladapter.h` (registry member, signal, slots)
- Modify: `websocket/uimanagercontroladapter.cpp` (`wantsPgmWait` branches at
  :262-264, :269-271, :390-392; remove `kControlPgmWaitTimeoutMs` (:14) and
  `resultForOperatorSeek` (:51+); keep `pgmTransactionDetails` (:26+) for the
  completion payload)
- Modify: `websocket/controlapiadapter.h` (two defaulted interface hooks)

**Interfaces:**
- Consumes: Task 3 registry, Task 4 `CommandResult::accepted`, Task 5 UIManager API.
- Produces (Task 7 relies on these exact names):
  - `ControlApiAdapter` gains defaulted hooks (fakes unaffected):
    ```cpp
    virtual QObject* completionNotifier() { return nullptr; }
    virtual void notifyClientDisconnected(const QString&) {}
    ```
  - `UIManagerControlAdapter::completionNotifier()` returns `this`;
    signal `void commandCompleted(const QString& clientId, const QString& commandId,
    const QJsonObject& completion);`
  - `notifyClientDisconnected(clientId)` → `m_registry->dropClient(clientId)`.
  - Completion payload from worker results:
    `{done:bool, reason?:"pgm_not_submitted"|"superseded", generation:"<str>",
    targetMs:double, pgmTransaction:{...pgmTransactionDetails...}, latencyMs:double}`.

- [ ] **Step 1: Implement the adapter changes**

`uimanagercontroladapter.h`:

```cpp
#include "pendingcommandregistry.h"

class UIManagerControlAdapter : public QObject, public ControlApiAdapter {
    Q_OBJECT
public:
    explicit UIManagerControlAdapter(UIManager* uiManager, QObject* parent = nullptr);
    // ... existing overrides ...
    QObject* completionNotifier() override { return this; }
    void notifyClientDisconnected(const QString& clientId) override;

signals:
    void commandCompleted(const QString& clientId, const QString& commandId,
                          const QJsonObject& completion);

private:
    void onOperatorSeekCompleted(quint64 workerEpoch, quint64 generation,
                                 const PlaybackWorker::OperatorSeekResult& result);
    PendingCommandRegistry* m_registry = nullptr;
    // ... existing members ...
};
```

`uimanagercontroladapter.cpp` constructor:

```cpp
UIManagerControlAdapter::UIManagerControlAdapter(UIManager* uiManager, QObject* parent)
    : QObject(parent), m_uiManager(uiManager), m_registry(new PendingCommandRegistry(this)) {
    connect(m_registry, &PendingCommandRegistry::commandCompleted, this,
            &UIManagerControlAdapter::commandCompleted);
    connect(m_registry, &PendingCommandRegistry::pendingExpired, this,
            [this](quint64 epoch, quint64 generation) {
                // A timed-out command must not trigger a late PGM dispatch.
                if (m_uiManager) m_uiManager->abandonOperatorSeek(epoch, generation);
            });
    if (m_uiManager) {
        connect(m_uiManager, &UIManager::operatorSeekCompleted, this,
                &UIManagerControlAdapter::onOperatorSeekCompleted);
        connect(m_uiManager, &UIManager::playbackWorkerEpochChanged, m_registry,
                &PendingCommandRegistry::noteWorkerEpoch);
    }
}
```

The completion mapper:

```cpp
void UIManagerControlAdapter::onOperatorSeekCompleted(
    quint64 workerEpoch, quint64 generation, const PlaybackWorker::OperatorSeekResult& result) {
    QJsonObject completion;
    const bool done = result.completed && result.submittedPgm;
    completion.insert(QStringLiteral("done"), done);
    if (!done) {
        completion.insert(QStringLiteral("reason"),
                          result.message == QStringLiteral("superseded")
                              ? QStringLiteral("superseded")
                              : QStringLiteral("pgm_not_submitted"));
    }
    completion.insert(QStringLiteral("generation"),
                      QString::number(static_cast<qulonglong>(generation)));
    completion.insert(QStringLiteral("targetMs"), static_cast<double>(result.targetMs));
    const QJsonObject details = pgmTransactionDetails(result);
    completion.insert(QStringLiteral("pgmTransaction"),
                      details.value(QStringLiteral("pgmTransaction")));
    m_registry->resolve(workerEpoch, generation, completion);
}

void UIManagerControlAdapter::notifyClientDisconnected(const QString& clientId) {
    m_registry->dropClient(clientId);
}
```

The three `wantsPgmWait` command branches — replace each blocking call. For
`transport.seek` (:269-271):

```cpp
        if (wantsPgmWait(args)) {
            const UIManager::AsyncSeekTicket ticket = m_uiManager->seekPlaybackAsyncPgm(positionMs);
            if (!ticket.accepted) {
                return CommandResult::failure(QStringLiteral("unavailable"), ticket.message);
            }
            m_registry->registerPending(ticket.workerEpoch, ticket.generation,
                                        args.value(QStringLiteral("_clientId")).toString(),
                                        args.value(QStringLiteral("_commandId")).toString());
            return CommandResult::accepted(ticket.workerEpoch, ticket.generation);
        }
        m_uiManager->seekPlayback(positionMs);
```

For `transport.stepFrame` (:262-264) and `action.jog` (:390-392), same shape with
`jogExternalAsyncPgm(frames)` / `jogExternalAsyncPgm(delta)`.

Delete `kControlPgmWaitTimeoutMs` and `resultForOperatorSeek` (now unused); keep
`pgmTransactionDetails` and the `OLR_E2E_LATENCY_TRACE` logging by moving the
`qInfo` block from `resultForOperatorSeek` into `onOperatorSeekCompleted` (same
fields, sourced from the completion result).

- [ ] **Step 2: Build + full unit label**

Run: `ninja -C build/gpuon && QT_QPA_PLATFORM=offscreen ctest --test-dir build/gpuon -L unit --output-on-failure`
Expected: build clean; suite green. (`uimanagercontroladapter.cpp` compiles only into
the app target — behavioral coverage of the mapping arrives with Task 7's server test
and the E2E; the registry logic it delegates to is already unit-tested.)

- [ ] **Step 3: Commit**

```bash
git add websocket/uimanagercontroladapter.h websocket/uimanagercontroladapter.cpp \
        websocket/controlapiadapter.h
git commit -m "control: accept transactional commands without blocking; correlate completions

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Server — serial clientId, `_commandId`, per-socket completion delivery

**Spec:** §4 (completion delivery, client identity, disconnect purge).

**Files:**
- Modify: `websocket/controlwebsocketserver.h` (members + slot)
- Modify: `websocket/controlwebsocketserver.cpp` (ctor; `handleNewConnection` :118-137;
  `handleTextMessage` `_clientId` injection at :164; `handleSocketDisconnected` :181)
- Test: `tests/unit/tst_controlwebsocketserver.cpp` (extends `ServerFakeAdapter`)

**Interfaces:**
- Consumes: Task 6 hooks (`completionNotifier`, `notifyClientDisconnected`,
  `commandCompleted` signal signature).
- Produces: wire behavior — each connection gets a unique serial `controlClientId`;
  every command's args carry `_commandId`; completions arrive as
  `{type:"event", name:"command.completed", data:{...completion, id:<commandId>}}`
  on the originating socket only.

- [ ] **Step 1: Write the failing test** (append to `tests/unit/tst_controlwebsocketserver.cpp`)

```cpp
// Extend ServerFakeAdapter with the completion hooks:
//   QObject* completionNotifier() override { return &notifier; }
//   void notifyClientDisconnected(const QString& clientId) override {
//       disconnectedClients.append(clientId);
//   }
//   CompletionNotifier notifier;           // tiny QObject declaring the signal:
//   QStringList disconnectedClients;
//
// class CompletionNotifier : public QObject {
//     Q_OBJECT
// signals:
//     void commandCompleted(const QString& clientId, const QString& commandId,
//                           const QJsonObject& completion);
// };

void TestControlWebSocketServer::completionEventReachesOnlyTheOriginatingClient() {
    ServerFakeAdapter adapter;
    adapter.resultDetails = QJsonObject{{QStringLiteral("status"), QStringLiteral("accepted")}};
    ControlWebSocketServer server(&adapter);
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QWebSocket clientA;
    QWebSocket clientB;
    // connect both to ws://127.0.0.1:<port>/control and wait for connected +
    // the initial snapshot/timecode messages, following the file's existing pattern.

    // Client A sends a transactional command; capture its ack.
    clientA.sendTextMessage(QStringLiteral(
        R"({"type":"command","id":"cmd-1","name":"transport.seek",)"
        R"("args":{"positionMs":1000,"waitForPgm":true}})"));
    // Wait for the ack on A (existing helper pattern) and record adapter.lastArgs.
    QTRY_VERIFY(!adapter.lastArgs.isEmpty());
    QCOMPARE(adapter.lastArgs.value(QStringLiteral("_commandId")).toString(),
             QStringLiteral("cmd-1"));
    const QString clientId = adapter.lastArgs.value(QStringLiteral("_clientId")).toString();
    QVERIFY(!clientId.isEmpty());

    // Fire the completion through the notifier; only A must receive the event.
    QSignalSpy aMessages(&clientA, &QWebSocket::textMessageReceived);
    QSignalSpy bMessages(&clientB, &QWebSocket::textMessageReceived);
    emit adapter.notifier.commandCompleted(
        clientId, QStringLiteral("cmd-1"),
        QJsonObject{{QStringLiteral("done"), true}, {QStringLiteral("latencyMs"), 12.0}});

    QTRY_VERIFY(aMessages.count() >= 1);
    const QJsonObject event =
        QJsonDocument::fromJson(aMessages.last().at(0).toString().toUtf8()).object();
    QCOMPARE(event.value(QStringLiteral("type")).toString(), QStringLiteral("event"));
    QCOMPARE(event.value(QStringLiteral("name")).toString(),
             QStringLiteral("command.completed"));
    const QJsonObject data = event.value(QStringLiteral("data")).toObject();
    QCOMPARE(data.value(QStringLiteral("id")).toString(), QStringLiteral("cmd-1"));
    QVERIFY(data.value(QStringLiteral("done")).toBool());
    QCOMPARE(bMessages.count(), 0);

    // Unknown clientId is silently dropped (no crash, nothing broadcast).
    emit adapter.notifier.commandCompleted(QStringLiteral("no-such-client"),
                                           QStringLiteral("cmd-2"), QJsonObject{});
    QTest::qWait(50);
    QCOMPARE(bMessages.count(), 0);

    // Disconnect purge notifies the adapter.
    clientA.close();
    QTRY_VERIFY(adapter.disconnectedClients.contains(clientId));
}
```

- [ ] **Step 2: Run to verify failure** —
  `ninja -C build/gpuon tst_controlwebsocketserver && QT_QPA_PLATFORM=offscreen ./build/gpuon/tests/unit/tst_controlwebsocketserver`
  Expected: FAIL (`completionNotifier` unknown on the fake until it overrides the
  Task 6 hook; no `_commandId`; no completion delivery).

- [ ] **Step 3: Implement**

`controlwebsocketserver.h` — members + private slot:

```cpp
    void deliverCommandCompleted(const QString& clientId, const QString& commandId,
                                 const QJsonObject& completion);
    QHash<QString, QWebSocket*> m_clientsById;
    quint64 m_nextClientSerial = 0;
```

`controlwebsocketserver.cpp`:

(a) Constructor — after `m_server` setup, connect the notifier if the adapter
provides one (signature match by name; the notifier is any QObject with the
`commandCompleted` signal):

```cpp
    if (m_adapter) {
        if (QObject* notifier = m_adapter->completionNotifier()) {
            connect(notifier,
                    SIGNAL(commandCompleted(QString, QString, QJsonObject)), this,
                    SLOT(deliverCommandCompleted(QString, QString, QJsonObject)));
        }
    }
```

(declare `deliverCommandCompleted` under a `private slots:` section for the
string-based connect, which decouples the server from the concrete adapter type).

(b) `handleNewConnection` — replace the pointer-derived id (:126):

```cpp
    const QString clientId = QString::number(++m_nextClientSerial);
    socket->setProperty("controlClientId", clientId);
    m_clientsById.insert(clientId, socket);
```

(c) `handleTextMessage` — next to the `_clientId` injection (:164):

```cpp
    commandArgs.insert(QStringLiteral("_commandId"), parsed.message.id);
```

(d) `handleSocketDisconnected` — before `m_sockets.remove(socket)`:

```cpp
    const QString clientId = socket->property("controlClientId").toString();
    m_clientsById.remove(clientId);
    if (m_adapter) m_adapter->notifyClientDisconnected(clientId);
```

(e) The delivery slot:

```cpp
void ControlWebSocketServer::deliverCommandCompleted(const QString& clientId,
                                                     const QString& commandId,
                                                     const QJsonObject& completion) {
    QWebSocket* socket = m_clientsById.value(clientId, nullptr);
    if (!socket) return; // client departed: drop, never re-route
    QJsonObject data = completion;
    data.insert(QStringLiteral("id"), commandId);
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("event"));
    event.insert(QStringLiteral("name"), QStringLiteral("command.completed"));
    event.insert(QStringLiteral("data"), data);
    sendJson(event, socket);
}
```

- [ ] **Step 4: Run the test + full unit label.** Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add websocket/controlwebsocketserver.h websocket/controlwebsocketserver.cpp \
        tests/unit/tst_controlwebsocketserver.cpp
git commit -m "control: deliver command.completed events to the originating client

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: E2E drivers + static guards migration

**Spec:** Backward compatibility (full consumer inventory), Testing contract (E2E).

**Files:**
- Modify: `tests/e2e/macos_app_driver.py` (`_command` ack wait at :135;
  `run_command_with_ndi_latency` at :446-521)
- Modify: `tests/e2e/ios_marker_srt_oracle.py` (`require_pgm_transaction` :606,
  `send_command_for_latency` :626-630, call sites :881-926)
- Modify: `tests/e2e/test_control_adapter_wait_static.py` (re-pin needles :16-33)

- [ ] **Step 1: Add a completion-aware command helper to `macos_app_driver.py`**
  (next to the existing ack reader at :135; reuse its message-read loop)

```python
def send_command_and_wait_completed(self, name, args=None, timeout=5.0):
    """Two-phase transactional command: returns (ack, completion).

    The ack arrives immediately with status="accepted"; the completion is the
    correlated command.completed event carrying the PGM outcome.
    """
    cmd_id = self._next_command_id()
    payload = {"type": "command", "id": cmd_id, "name": name, "args": args or {}}
    self._send_json(payload)
    ack = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        msg = self._read_message(timeout=deadline - time.monotonic())
        if msg.get("type") == "ack" and msg.get("id") == cmd_id:
            if not msg.get("ok"):
                raise AssertionError(f"{name}: command rejected: {msg}")
            ack = msg
            continue
        if (msg.get("type") == "event" and msg.get("name") == "command.completed"
                and msg.get("data", {}).get("id") == cmd_id):
            if ack is None:
                raise AssertionError(f"{name}: completion before ack: {msg}")
            return ack, msg["data"]
    raise TimeoutError(f"timed out waiting for command.completed for {name}")
```

(Adapt `_next_command_id` / `_send_json` / `_read_message` to the module's actual
helper names — the ack-wait loop at :135 shows the read pattern to reuse.)

- [ ] **Step 2: Migrate `run_command_with_ndi_latency`** — the `command()` callable
  for `waitForPgm` commands now returns `(ack, completion)`:
  - `pgm_transaction` comes from `completion["pgmTransaction"]` (was
    `command_ack["pgmTransaction"]`, :505);
  - the strict assertion (`ack_after_pgm_transaction`, :506-515) becomes: the
    *completion* must exist with `done == true`;
  - add the non-blocking assertion: the accepted **ack** must round-trip fast even
    when the marker lands later — record `ack_elapsed_ms` at ack receipt and assert
    `ack_elapsed_ms < 100` for every transactional command in strict mode (the ack
    no longer waits on decode/PGM; 100 ms is generous for a localhost WS
    round-trip under CI load while still catching any reintroduced blocking wait);
  - keep the NDI marker latency gate exactly as-is (authoritative PGM latency).

- [ ] **Step 3: Migrate the iOS oracle** — in `ios_marker_srt_oracle.py`:
  - `send_command_for_latency` (:626) keeps setting `waitForPgm: True` and now waits
    for the completion event (same two-phase helper pattern as Step 1);
  - `require_pgm_transaction(ack, label)` (:606) takes the completion `data` instead
    of the ack: `transaction = data.get("pgmTransaction")`; the call sites at
    :884/:902/:926 pass the completion;
  - the latency log lines (:673-683) source `pgmTransactionElapsedNs`-equivalents
    from the completion (`latencyMs`) — rename the logged field to
    `completionLatencyMs` to stay truthful.

- [ ] **Step 4: Re-pin the static guard** — `test_control_adapter_wait_static.py`
  asserts the new contract on `websocket/uimanagercontroladapter.cpp`:

```python
require(source, "waitForPgm", "operator PGM wait mode must be explicit")
require(source, "seekPlaybackAsyncPgm", "transport.seek must use the async PGM path")
require(source, "jogExternalAsyncPgm", "step/jog must use the async PGM path")
require(source, "registerPending", "transactional commands must register a pending completion")
forbid(source, "seekPlaybackAndWaitForPgm", "the adapter must never block on PGM")
forbid(source, "jogExternalAndWaitForPgm", "the adapter must never block on PGM")
```

(add a `forbid(source, needle, msg)` helper mirroring `require`; also require
`"wirePlaybackWorkerCompletion"` in `uimanager.cpp` — the wiring guard for Task 5 —
and keep the file's existing branch-order checks by updating their needles from the
blocking calls to the async ones.)

- [ ] **Step 5: Verify the static tests + drivers parse**

Run:
```sh
python3 tests/e2e/test_control_adapter_wait_static.py websocket/uimanagercontroladapter.cpp
python3 -m py_compile tests/e2e/macos_app_driver.py tests/e2e/ios_marker_srt_oracle.py
python3 tests/e2e/test_macos_app_driver_static.py tests/e2e/macos_app_driver.py
python3 tests/e2e/test_ios_marker_oracle_static.py tests/e2e/ios_marker_srt_oracle.py
```
Expected: all PASS (update `test_macos_app_driver_static.py` /
`test_ios_marker_oracle_static.py` needles in the same edit if they pin the old
ack-shape strings — check with `grep -n pgmTransaction` first).

- [ ] **Step 6: Run the macOS app E2E** (requires local NDI runtime + WindowServer;
  skip cleanly if unavailable and note it in the task report)

Run: `ctest --test-dir build/gpuon -R e2e_macos_app_visual --output-on-failure`
Expected: `APP_E2E_PASS`, all NDI latency samples within the existing gate, and the
new fast-ack assertions passing.

- [ ] **Step 7: Commit**

```bash
git add tests/e2e/macos_app_driver.py tests/e2e/ios_marker_srt_oracle.py \
        tests/e2e/test_control_adapter_wait_static.py \
        tests/e2e/test_macos_app_driver_static.py tests/e2e/test_ios_marker_oracle_static.py
git commit -m "test: migrate PGM drivers to two-phase command completion

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Integrated verification

**Files:** none (verification only).

- [ ] **Step 1: Full builds** — `ninja -C build/gpuon && ninja -C build/gpuoff`
  Expected: both `-Werror` clean.

- [ ] **Step 2: Full unit label** —
  `QT_QPA_PLATFORM=offscreen ctest --test-dir build/gpuon -L unit --output-on-failure`
  Expected: 100% pass.

- [ ] **Step 3: Playback E2E sanity** —
  `ctest --test-dir build/gpuon -R 'e2e_play' --output-on-failure` for
  `play1x seekplay stepscrub` (worker seek-path changes must not disturb desktop
  playback scenarios).

- [ ] **Step 4: macOS app E2E** (if not run in Task 8):
  `ctest --test-dir build/gpuon -R e2e_macos_app_visual --output-on-failure`
  Expected: `APP_E2E_PASS`; record max NDI marker latency and max accepted-ack
  round-trip in the task report. Evidence discipline: do NOT claim latency
  improvements — only that the gates pass and the ack no longer waits on PGM.

- [ ] **Step 5: Format check** —
  `python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --diff --commit origin/main --extensions cpp,h,hpp,mm,c`
  Expected: no modifications needed.

- [ ] **Step 6: Independent review gate** — this plan touches the playback worker's
  threading (Tasks 1–2): per repo policy, request an independent review before
  merging. Do not push without the pre-push hook.

---

## Execution notes

- Tasks 1–4 are independent of the UI layer and land first; Task 5 wires UIManager;
  Tasks 6–7 flip the WebSocket behavior; Task 8 migrates every consumer in the same
  branch so no intermediate state leaves the E2E drivers broken. Do not reorder
  6/7 ahead of 8's static-guard update within a push (the pre-push e2e static checks
  would fail on the old needles).
- The `operatorSeekCompleted` emissions run under `m_mutex`; every consumer
  connection is explicit `Qt::QueuedConnection`. If any test needs synchronous
  observation, drain with `QTRY_*`/`processEvents`, never a direct connection.
- `tst_playbackworker` runs in the tsan-gpu CI leg (added earlier on this branch):
  the new signal emissions get TSan coverage automatically.

## Self-review (performed at write time)

- Spec coverage: Problem/Goal→Tasks 6–7; §1→Task 2 (+5); §2→Task 1; §3→Tasks 3, 5, 6;
  §4→Task 7; Scope decision→Tasks 6, 8; Protocol→Tasks 4, 7; Semantics (four
  outcomes)→Tasks 3 (timeout/superseded), 6 (done/pgm_not_submitted); Backward
  compat inventory→Task 8; Testing contract→each task's tests + Task 9. No uncovered
  section.
- Placeholder scan: the two harness ellipses in Task 2's tests explicitly reference
  the concrete Task 1 test to copy; all other steps carry complete code.
- Type consistency: `operatorSeekCompleted(quint64, PlaybackWorker::OperatorSeekResult)`
  (worker) vs `(quint64, quint64, PlaybackWorker::OperatorSeekResult)` (UIManager
  relay) used consistently in Tasks 1/5/6; `AsyncSeekTicket` fields match between
  Tasks 5 and 6; registry API names match between Tasks 3, 6, 7;
  `CommandResult::accepted(quint64, quint64)` matches Tasks 4 and 6.
