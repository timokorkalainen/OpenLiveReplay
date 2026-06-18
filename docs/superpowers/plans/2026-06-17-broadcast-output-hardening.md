# Broadcast Output Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make playback clean outputs measurable, continuous, and diagnosable enough to support professional live TV broadcast use, starting with output-clock/deadline telemetry and sink backpressure.

**Architecture:** The app-generated `OutputBusFrame` timeline remains the single source of truth for Qt preview, NDI, DeckLink, ST 2110, OMT, and future AJA outputs. This plan hardens the shared output runtime first: `OutputRuntime` owns cadence/deadline telemetry, `OutputDispatcher` owns per-target frame identity/health, `IOutputSink` implementations report delivery/backpressure status, and UI maps those signals into operator health without changing frame production. Later backend-specific PRs consume the same telemetry contract.

**Tech Stack:** C++17, Qt 6 (`QThread`, `QElapsedTimer`, `QMutex`, QML), Qt Test/CTest, existing playback/output classes (`OutputRuntime`, `OutputDispatcher`, `QueuedOutputSink`, `NdiOutputSink`, `BroadcastOutputSettings`).

---

## Scope

This plan is a multi-PR broadcast-output hardening program. The first implementation PR is fully specified below and must be completed first:

1. **PR 1: Output clock deadline and backpressure telemetry** — shared runtime/sink diagnostics, status mapping, and tests.
2. PR 2: NDI sender validation, recovery semantics, and runtime-soak observability.
3. PR 3: Rational playback sampling/stepping for 29.97 and 59.94 outputs.
4. PR 4: Operator health model polish and long-run output confidence harnesses.
5. PR 5+: DeckLink SDI/HDMI, DeckLink IP/ST 2110, and later AJA/OMT backends using the same output contract.

PR 1 deliberately does **not** add DeckLink/ST 2110 APIs, rework the recorder/input side, or change Qt rendering behavior. It adds the measurement layer that tells us when the output engine is or is not keeping frame-perfect cadence.

## Files And Responsibilities

- Modify `playback/output/outputdispatcher.h` / `.cpp`: extend `OutputDispatchStats` and `OutputTargetDispatchStats` with runtime cadence, sink queue, and per-target delivery continuity fields.
- Modify `playback/output/outputruntime.h` / `.cpp`: measure due ticks, actual dispatch timing, catch-up caps, deadline misses, and last runtime tick health.
- Modify `playback/output/outputsink.h`: extend `OutputSinkStatus` with queue depth, max depth, last queued/delivered frame indexes, delivery gaps, and submit latency.
- Modify `playback/output/queuedoutputsink.h` / `.cpp`: track queue depth/backpressure and delivery index continuity around the async inner sink.
- Modify `playback/output/ndisink.h` / `.cpp`: expose last send duration and last frame identity through existing status objects.
- Modify `playback/output/broadcastoutputsettings.h` / `.cpp`: surface new diagnostics and map deadline/backpressure/gap failures into `Active`, `Degraded`, or `Error`.
- Modify `playback/output/broadcastoutputstatus.cpp`: copy new stats from dispatcher to UI status rows.
- Modify `uimanager.cpp`: include new telemetry fields in the output-status fingerprint.
- Modify `Main.qml`: show compact deadline/backpressure diagnostics in the existing NDI Outputs table.
- Modify tests:
  - `tests/unit/tst_outputruntime.cpp`
  - `tests/unit/tst_outputdispatcher.cpp`
  - `tests/unit/tst_queuedoutputsink.cpp`
  - `tests/unit/tst_ndisink.cpp`
  - `tests/unit/tst_broadcastoutputsettings.cpp`

## PR 1 Acceptance Criteria

- Output runtime reports deadline misses when wall-clock time advances faster than the allowed catch-up window.
- Normal paused playback still emits continuous output frame indexes and reports no deadline misses.
- Queued sink reports current queue depth, max queue depth, dropped frames, and delivery frame-index gaps.
- NDI sink reports last send duration and keeps last frame identity in status.
- Broadcast status rows preserve cumulative diagnostics but current severity is based on current output health:
  - deadline miss or delivery gap: `Error`
  - queue pressure or dropped queued frames: `Degraded` unless current delivery is failing, then `Error`
  - repeated payload during pause: not degraded by itself
  - silent audio: not degraded by itself
  - placeholder video: `Degraded`
- Existing output behavior remains frame-producing and continuous; telemetry must not block the output runtime.

---

### Task 1: Add Runtime Deadline Telemetry

**Files:**
- Modify: `playback/output/outputdispatcher.h`
- Modify: `playback/output/outputruntime.h`
- Modify: `playback/output/outputruntime.cpp`
- Test: `tests/unit/tst_outputruntime.cpp`

- [ ] **Step 1: Write failing tests for normal cadence and catch-up cap**

Add two test slots to `TestOutputRuntime` in `tests/unit/tst_outputruntime.cpp`:

```cpp
private slots:
    void manualTicksRepeatPausedFrameFromCache();
    void nanosecondTicksHonorFractionalFrameBoundary();
    void workerThreadTicksWithoutExternalDispatchCalls();
    void runtimeStatsReportNoDeadlineMissForOnTimeTicks();
    void runtimeStatsReportDeadlineMissWhenCatchUpIsCapped();
```

Add these test bodies:

```cpp
void TestOutputRuntime::runtimeStatsReportNoDeadlineMissForOnTimeTicks() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 40));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    ThreadSafeCollectingSink sink(OutputTargetKind::QtPreview);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    runtime.dispatchDueTicksForTestNs(0);
    runtime.dispatchDueTicksForTestNs(40000000);
    runtime.dispatchDueTicksForTestNs(80000000);

    const OutputDispatchStats stats = runtime.stats();
    QCOMPARE(stats.ticks, qint64(3));
    QCOMPARE(stats.runtime.deadlineMisses, qint64(0));
    QCOMPARE(stats.runtime.catchUpCapHits, qint64(0));
    QCOMPARE(stats.runtime.lastDispatchedFrameIndex, qint64(2));
    QVERIFY(stats.runtime.hasLastDispatchTiming);
    QVERIFY(stats.runtime.lastLatenessNs <= 0);
}

void TestOutputRuntime::runtimeStatsReportDeadlineMissWhenCatchUpIsCapped() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 44));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    ThreadSafeCollectingSink sink(OutputTargetKind::QtPreview);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    runtime.dispatchDueTicksForTestNs(0);
    runtime.dispatchDueTicksForTestNs(600000000);

    const OutputDispatchStats stats = runtime.stats();
    QCOMPARE(stats.ticks, qint64(9)); // first tick plus max catch-up batch of 8
    QVERIFY(stats.runtime.deadlineMisses > 0);
    QVERIFY(stats.runtime.catchUpCapHits > 0);
    QVERIFY(stats.runtime.maxLatenessNs > 0);
    QCOMPARE(stats.runtime.lastDispatchedFrameIndex, qint64(8));
}
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
cmake --build build --target tst_outputruntime
ctest --test-dir build -R '^tst_outputruntime$' --output-on-failure
```

Expected: build fails because `OutputDispatchStats::runtime` and its fields do not exist.

- [ ] **Step 3: Add runtime telemetry data structures**

In `playback/output/outputdispatcher.h`, add:

```cpp
struct OutputRuntimeDispatchStats {
    bool hasLastDispatchTiming = false;
    qint64 lastScheduledFrameIndex = -1;
    qint64 lastDispatchedFrameIndex = -1;
    qint64 lastScheduledNs = 0;
    qint64 lastDispatchWallNs = 0;
    qint64 lastLatenessNs = 0;
    qint64 maxLatenessNs = 0;
    qint64 deadlineMisses = 0;
    qint64 catchUpCapHits = 0;
    qint64 cappedCatchUpTicks = 0;
};
```

Extend `OutputDispatchStats`:

```cpp
struct OutputDispatchStats {
    qint64 ticks = 0;
    qint64 framesSubmitted = 0;
    qint64 sinkFailures = 0;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
    OutputRuntimeDispatchStats runtime;
    QHash<QString, OutputTargetDispatchStats> targets;
};
```

- [ ] **Step 4: Add runtime telemetry writer**

In `playback/output/outputruntime.h`, add private helper declaration:

```cpp
void recordDispatchTiming(qint64 outputFrameIndex, qint64 scheduledNs, qint64 wallNowNs);
```

In `playback/output/outputruntime.cpp`, add:

```cpp
void OutputRuntime::recordDispatchTiming(qint64 outputFrameIndex, qint64 scheduledNs,
                                         qint64 wallNowNs) {
    OutputDispatchStats stats = m_dispatcher.stats();
    OutputRuntimeDispatchStats runtime = stats.runtime;
    const qint64 latenessNs = wallNowNs - scheduledNs;
    runtime.hasLastDispatchTiming = true;
    runtime.lastScheduledFrameIndex = outputFrameIndex;
    runtime.lastDispatchedFrameIndex = outputFrameIndex;
    runtime.lastScheduledNs = scheduledNs;
    runtime.lastDispatchWallNs = wallNowNs;
    runtime.lastLatenessNs = latenessNs;
    runtime.maxLatenessNs = qMax(runtime.maxLatenessNs, latenessNs);
    if (latenessNs > 0) runtime.deadlineMisses++;
    m_dispatcher.setRuntimeStats(runtime);
}
```

Also add `void setRuntimeStats(const OutputRuntimeDispatchStats& stats);` to `OutputDispatcher` and implement:

```cpp
void OutputDispatcher::setRuntimeStats(const OutputRuntimeDispatchStats& stats) {
    m_stats.runtime = stats;
}
```

- [ ] **Step 5: Wire timing in `dispatchDueTicksNs`**

In `OutputRuntime::dispatchDueTicksNs`, before dispatching a frame, compute the scheduled frame index and scheduled time while holding the runtime lock:

```cpp
qint64 frameIndex = 0;
qint64 scheduledNs = 0;
{
    QMutexLocker locker(&m_mutex);
    if (m_stopRequested) break;
    const FrameRate rate = m_dispatcher.frameRate();
    frameIndex = m_dispatcher.nextOutputFrameIndex();
    scheduledNs = frameIndexToNsCeil(rate, frameIndex);
    if (!rate.isValid() || scheduledNs > elapsedNs) {
        return m_dispatcher.stats();
    }
}
```

After `m_dispatcher.dispatchTick(current.cache, current.state);`, call:

```cpp
recordDispatchTiming(frameIndex, scheduledNs, elapsedNs);
```

When the `while (dispatched < m_maxCatchUpTicks)` loop exits because `dispatched == m_maxCatchUpTicks`, update runtime stats:

```cpp
{
    QMutexLocker locker(&m_mutex);
    OutputRuntimeDispatchStats runtime = m_dispatcher.stats().runtime;
    runtime.catchUpCapHits++;
    runtime.cappedCatchUpTicks++;
    m_dispatcher.setRuntimeStats(runtime);
    return m_dispatcher.stats();
}
```

- [ ] **Step 6: Run tests to verify GREEN**

Run:

```bash
cmake --build build --target tst_outputruntime
ctest --test-dir build -R '^tst_outputruntime$' --output-on-failure
```

Expected: `tst_outputruntime` passes.

- [ ] **Step 7: Commit**

```bash
git add playback/output/outputdispatcher.h playback/output/outputdispatcher.cpp playback/output/outputruntime.h playback/output/outputruntime.cpp tests/unit/tst_outputruntime.cpp
git commit -m "add output runtime deadline telemetry"
```

---

### Task 2: Add Queue Backpressure And Delivery Continuity Status

**Files:**
- Modify: `playback/output/outputsink.h`
- Modify: `playback/output/queuedoutputsink.h`
- Modify: `playback/output/queuedoutputsink.cpp`
- Test: `tests/unit/tst_queuedoutputsink.cpp`

- [ ] **Step 1: Write failing tests for queue pressure and delivery gaps**

Add test slots in `TestQueuedOutputSink`:

```cpp
void queueStatusReportsDepthAndDroppedFrames();
void deliveryGapsAreVisibleInStatus();
```

Add a helper sink that records frame indexes and can reject a specific frame:

```cpp
class GapReportingInnerSink final : public IOutputSink {
public:
    explicit GapReportingInnerSink(qint64 rejectedIndex = -1) : m_rejectedIndex(rejectedIndex) {}

    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        QMutexLocker locker(&m_mutex);
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }

    void stop() override {
        QMutexLocker locker(&m_mutex);
        m_active = false;
    }

    bool isActive() const override {
        QMutexLocker locker(&m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame& frame) override {
        QMutexLocker locker(&m_mutex);
        if (!m_active) return false;
        m_attempted.append(frame.outputFrameIndex);
        if (frame.outputFrameIndex == m_rejectedIndex) return false;
        m_delivered.append(frame.outputFrameIndex);
        return true;
    }

    QVector<qint64> delivered() const {
        QMutexLocker locker(&m_mutex);
        return m_delivered;
    }

private:
    mutable QMutex m_mutex;
    bool m_active = false;
    qint64 m_rejectedIndex = -1;
    QVector<qint64> m_attempted;
    QVector<qint64> m_delivered;
};
```

Add tests:

```cpp
void TestQueuedOutputSink::queueStatusReportsDepthAndDroppedFrames() {
    auto inner = std::make_unique<SlowCollectingSink>();
    QueuedOutputSink sink(std::move(inner), 1);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(10)));
    QVERIFY(sink.submit(frame(11)));
    QVERIFY(sink.outputStatus().maxQueueDepth >= 1);
    QVERIFY(sink.outputStatus().droppedFrames >= 1);
    QCOMPARE(sink.outputStatus().lastQueuedFrameIndex, qint64(11));

    sink.stop();
}

void TestQueuedOutputSink::deliveryGapsAreVisibleInStatus() {
    auto inner = std::make_unique<GapReportingInnerSink>();
    GapReportingInnerSink* observed = inner.get();
    QueuedOutputSink sink(std::move(inner), 3);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(20)));
    QVERIFY(sink.submit(frame(22)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->delivered().size(), 2, 500);
    const OutputSinkStatus status = sink.outputStatus();
    QVERIFY(status.deliveryGaps > 0);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(22));

    sink.stop();
}
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
cmake --build build --target tst_queuedoutputsink
ctest --test-dir build -R '^tst_queuedoutputsink$' --output-on-failure
```

Expected: build fails because `OutputSinkStatus::maxQueueDepth`, `lastQueuedFrameIndex`, `lastDeliveredFrameIndex`, and `deliveryGaps` do not exist.

- [ ] **Step 3: Extend `OutputSinkStatus`**

In `playback/output/outputsink.h`, extend `OutputSinkStatus`:

```cpp
qint64 currentQueueDepth = 0;
qint64 maxQueueDepth = 0;
qint64 deliveryGaps = 0;
qint64 lastQueuedFrameIndex = -1;
qint64 lastDeliveredFrameIndex = -1;
qint64 lastSubmitDurationNs = 0;
bool hasLastQueuedFrameIndex = false;
bool hasLastDeliveredFrameIndex = false;
```

- [ ] **Step 4: Track queue and delivery status in `QueuedOutputSink`**

In `playback/output/queuedoutputsink.h`, add fields:

```cpp
qint64 m_maxQueueDepth = 0;
qint64 m_deliveryGaps = 0;
qint64 m_lastQueuedFrameIndex = -1;
qint64 m_lastDeliveredFrameIndex = -1;
bool m_hasLastQueuedFrameIndex = false;
bool m_hasLastDeliveredFrameIndex = false;
```

Reset them in `QueuedOutputSink::start()`:

```cpp
m_maxQueueDepth = 0;
m_deliveryGaps = 0;
m_lastQueuedFrameIndex = -1;
m_lastDeliveredFrameIndex = -1;
m_hasLastQueuedFrameIndex = false;
m_hasLastDeliveredFrameIndex = false;
```

Update `QueuedOutputSink::submit()` after appending:

```cpp
m_lastQueuedFrameIndex = frame.outputFrameIndex;
m_hasLastQueuedFrameIndex = true;
m_maxQueueDepth = qMax<qint64>(m_maxQueueDepth, m_queue.size());
```

Update `QueuedOutputSink::workerLoop()` after `m_inner->submit(frame)`:

```cpp
if (submitted) {
    if (m_hasLastDeliveredFrameIndex &&
        frame.outputFrameIndex != m_lastDeliveredFrameIndex + 1) {
        m_deliveryGaps++;
    }
    m_lastDeliveredFrameIndex = frame.outputFrameIndex;
    m_hasLastDeliveredFrameIndex = true;
}
```

Update `QueuedOutputSink::outputStatus()` inside the lock:

```cpp
own.currentQueueDepth = m_queue.size();
own.maxQueueDepth = m_maxQueueDepth;
own.deliveryGaps = m_deliveryGaps;
own.lastQueuedFrameIndex = m_lastQueuedFrameIndex;
own.lastDeliveredFrameIndex = m_lastDeliveredFrameIndex;
own.hasLastQueuedFrameIndex = m_hasLastQueuedFrameIndex;
own.hasLastDeliveredFrameIndex = m_hasLastDeliveredFrameIndex;
```

When merging `inner` status, combine:

```cpp
own.currentQueueDepth += inner.currentQueueDepth;
own.maxQueueDepth = qMax(own.maxQueueDepth, inner.maxQueueDepth);
own.deliveryGaps += inner.deliveryGaps;
if (inner.hasLastQueuedFrameIndex) {
    own.hasLastQueuedFrameIndex = true;
    own.lastQueuedFrameIndex = inner.lastQueuedFrameIndex;
}
if (inner.hasLastDeliveredFrameIndex) {
    own.hasLastDeliveredFrameIndex = true;
    own.lastDeliveredFrameIndex = inner.lastDeliveredFrameIndex;
}
own.lastSubmitDurationNs = qMax(own.lastSubmitDurationNs, inner.lastSubmitDurationNs);
```

- [ ] **Step 5: Run tests to verify GREEN**

Run:

```bash
cmake --build build --target tst_queuedoutputsink
ctest --test-dir build -R '^tst_queuedoutputsink$' --output-on-failure
```

Expected: `tst_queuedoutputsink` passes.

- [ ] **Step 6: Commit**

```bash
git add playback/output/outputsink.h playback/output/queuedoutputsink.h playback/output/queuedoutputsink.cpp tests/unit/tst_queuedoutputsink.cpp
git commit -m "report output sink queue backpressure"
```

---

### Task 3: Propagate Runtime And Sink Telemetry To Broadcast Status

**Files:**
- Modify: `playback/output/outputdispatcher.h`
- Modify: `playback/output/outputdispatcher.cpp`
- Modify: `playback/output/broadcastoutputsettings.h`
- Modify: `playback/output/broadcastoutputsettings.cpp`
- Modify: `playback/output/broadcastoutputstatus.cpp`
- Modify: `uimanager.cpp`
- Test: `tests/unit/tst_outputdispatcher.cpp`
- Test: `tests/unit/tst_broadcastoutputsettings.cpp`

- [ ] **Step 1: Write failing status propagation tests**

In `tests/unit/tst_outputdispatcher.cpp`, extend `statsMergeSinkOutputStatusWithDispatchAttempts()`:

```cpp
status.currentQueueDepth = 1;
status.maxQueueDepth = 3;
status.deliveryGaps = 2;
status.lastQueuedFrameIndex = 12;
status.lastDeliveredFrameIndex = 11;
status.hasLastQueuedFrameIndex = true;
status.hasLastDeliveredFrameIndex = true;
```

Add assertions:

```cpp
QCOMPARE(target.currentQueueDepth, qint64(1));
QCOMPARE(target.maxQueueDepth, qint64(3));
QCOMPARE(target.deliveryGaps, qint64(2));
QCOMPARE(target.lastQueuedFrameIndex, qint64(12));
QCOMPARE(target.lastDeliveredFrameIndex, qint64(11));
QVERIFY(target.hasLastQueuedFrameIndex);
QVERIFY(target.hasLastDeliveredFrameIndex);
```

In `tests/unit/tst_broadcastoutputsettings.cpp`, add slot:

```cpp
void rowsMarkDeadlineMissAndDeliveryGapAsError();
```

Add body:

```cpp
void TestBroadcastOutputSettings::rowsMarkDeadlineMissAndDeliveryGapAsError() {
    QList<OutputTargetAssignment> outputs = BroadcastOutputSettings::setEnabled(
        {}, 1, OutputTargetKind::Ndi, OutputBusId::feed(0), true);

    BroadcastOutputTargetStatus status;
    status.attemptedFrames = 5;
    status.framesSubmitted = 5;
    status.hasLastSubmitResult = true;
    status.lastSubmitSucceeded = true;
    status.runtimeDeadlineMisses = 1;
    status.deliveryGaps = 1;
    status.hasLastIdentity = true;
    status.lastIdentity.bus = OutputBusId::feed(0);
    status.lastIdentity.outputFrameIndex = 4;

    QHash<QString, BroadcastOutputTargetStatus> statuses;
    statuses.insert(QStringLiteral("feed0-ndi"), status);

    const QVariantMap row =
        BroadcastOutputSettings::rows(outputs, 1, OutputTargetKind::Ndi, statuses)[0].toMap();

    QCOMPARE(row.value(QStringLiteral("statusState")).toString(), QStringLiteral("Error"));
    QCOMPARE(row.value(QStringLiteral("statusSeverity")).toString(), QStringLiteral("error"));
    QVERIFY(row.value(QStringLiteral("diagnostic")).toString().contains(QStringLiteral("deadline=1")));
    QVERIFY(row.value(QStringLiteral("diagnostic")).toString().contains(QStringLiteral("gap=1")));
}
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
cmake --build build --target tst_outputdispatcher tst_broadcastoutputsettings
ctest --test-dir build -R '^(tst_outputdispatcher|tst_broadcastoutputsettings)$' --output-on-failure
```

Expected: build fails because the new fields do not exist.

- [ ] **Step 3: Extend target dispatch stats**

In `playback/output/outputdispatcher.h`, add to `OutputTargetDispatchStats`:

```cpp
qint64 currentQueueDepth = 0;
qint64 maxQueueDepth = 0;
qint64 deliveryGaps = 0;
qint64 lastQueuedFrameIndex = -1;
qint64 lastDeliveredFrameIndex = -1;
qint64 lastSubmitDurationNs = 0;
bool hasLastQueuedFrameIndex = false;
bool hasLastDeliveredFrameIndex = false;
```

In `OutputDispatcher::stats()`, copy these fields from `sinkStatus`:

```cpp
target.currentQueueDepth = sinkStatus.currentQueueDepth;
target.maxQueueDepth = sinkStatus.maxQueueDepth;
target.deliveryGaps = sinkStatus.deliveryGaps;
target.lastQueuedFrameIndex = sinkStatus.lastQueuedFrameIndex;
target.lastDeliveredFrameIndex = sinkStatus.lastDeliveredFrameIndex;
target.lastSubmitDurationNs = sinkStatus.lastSubmitDurationNs;
target.hasLastQueuedFrameIndex = sinkStatus.hasLastQueuedFrameIndex;
target.hasLastDeliveredFrameIndex = sinkStatus.hasLastDeliveredFrameIndex;
```

- [ ] **Step 4: Extend broadcast status model**

In `playback/output/broadcastoutputsettings.h`, add to `BroadcastOutputTargetStatus`:

```cpp
qint64 currentQueueDepth = 0;
qint64 maxQueueDepth = 0;
qint64 deliveryGaps = 0;
qint64 lastQueuedFrameIndex = -1;
qint64 lastDeliveredFrameIndex = -1;
qint64 lastSubmitDurationNs = 0;
qint64 runtimeDeadlineMisses = 0;
qint64 runtimeCatchUpCapHits = 0;
bool hasLastQueuedFrameIndex = false;
bool hasLastDeliveredFrameIndex = false;
```

In `BroadcastOutputStatus::fromDispatchStats()`, copy target queue fields and runtime aggregate fields:

```cpp
status.currentQueueDepth = source.currentQueueDepth;
status.maxQueueDepth = source.maxQueueDepth;
status.deliveryGaps = source.deliveryGaps;
status.lastQueuedFrameIndex = source.lastQueuedFrameIndex;
status.lastDeliveredFrameIndex = source.lastDeliveredFrameIndex;
status.lastSubmitDurationNs = source.lastSubmitDurationNs;
status.hasLastQueuedFrameIndex = source.hasLastQueuedFrameIndex;
status.hasLastDeliveredFrameIndex = source.hasLastDeliveredFrameIndex;
status.runtimeDeadlineMisses = stats.runtime.deadlineMisses;
status.runtimeCatchUpCapHits = stats.runtime.catchUpCapHits;
```

- [ ] **Step 5: Update status severity and diagnostics**

In `broadcastoutputsettings.cpp`, update `statusState()` and `statusSeverity()` so this condition is `Error`:

```cpp
(status->runtimeDeadlineMisses > 0 || status->deliveryGaps > 0)
```

Add this condition as `Degraded` / `warning` when no error condition is active:

```cpp
(status->maxQueueDepth > 0 || status->sinkDroppedFrames > 0)
```

Extend `diagnosticText()` format with:

```cpp
" deadline=%13 cap=%14 q=%15 maxQ=%16 gap=%17 lastQueued=%18 lastDelivered=%19 sendNs=%20"
```

Append arguments:

```cpp
.arg(status->runtimeDeadlineMisses)
.arg(status->runtimeCatchUpCapHits)
.arg(status->currentQueueDepth)
.arg(status->maxQueueDepth)
.arg(status->deliveryGaps)
.arg(valueOrDash(status->lastQueuedFrameIndex, status->hasLastQueuedFrameIndex))
.arg(valueOrDash(status->lastDeliveredFrameIndex, status->hasLastDeliveredFrameIndex))
.arg(status->lastSubmitDurationNs)
```

Add row fields in `addStatusFields()`:

```cpp
row.insert(QStringLiteral("currentQueueDepth"), values.currentQueueDepth);
row.insert(QStringLiteral("maxQueueDepth"), values.maxQueueDepth);
row.insert(QStringLiteral("deliveryGaps"), values.deliveryGaps);
row.insert(QStringLiteral("lastQueuedFrameIndex"), values.lastQueuedFrameIndex);
row.insert(QStringLiteral("lastDeliveredFrameIndex"), values.lastDeliveredFrameIndex);
row.insert(QStringLiteral("lastSubmitDurationNs"), values.lastSubmitDurationNs);
row.insert(QStringLiteral("runtimeDeadlineMisses"), values.runtimeDeadlineMisses);
row.insert(QStringLiteral("runtimeCatchUpCapHits"), values.runtimeCatchUpCapHits);
```

- [ ] **Step 6: Include new fields in UI status fingerprint**

In `uimanager.cpp`, extend `outputStatusFingerprint()` target mixing:

```cpp
mix(quint64(target.currentQueueDepth));
mix(quint64(target.maxQueueDepth));
mix(quint64(target.deliveryGaps));
mix(quint64(target.lastQueuedFrameIndex));
mix(quint64(target.lastDeliveredFrameIndex));
mix(quint64(target.lastSubmitDurationNs));
mix(target.hasLastQueuedFrameIndex ? 1 : 0);
mix(target.hasLastDeliveredFrameIndex ? 1 : 0);
```

Also mix runtime aggregate fields before target loop:

```cpp
mix(quint64(stats.runtime.deadlineMisses));
mix(quint64(stats.runtime.catchUpCapHits));
mix(quint64(stats.runtime.cappedCatchUpTicks));
mix(quint64(stats.runtime.lastDispatchedFrameIndex));
mix(quint64(stats.runtime.lastLatenessNs));
mix(quint64(stats.runtime.maxLatenessNs));
```

- [ ] **Step 7: Run tests to verify GREEN**

Run:

```bash
cmake --build build --target tst_outputdispatcher tst_broadcastoutputsettings OpenLiveReplay
ctest --test-dir build -R '^(tst_outputdispatcher|tst_broadcastoutputsettings|qml_smoke)$' --output-on-failure
```

Expected: all listed tests pass.

- [ ] **Step 8: Commit**

```bash
git add playback/output/outputdispatcher.h playback/output/outputdispatcher.cpp playback/output/broadcastoutputsettings.h playback/output/broadcastoutputsettings.cpp playback/output/broadcastoutputstatus.cpp uimanager.cpp tests/unit/tst_outputdispatcher.cpp tests/unit/tst_broadcastoutputsettings.cpp
git commit -m "surface output deadline and queue health"
```

---

### Task 4: Add NDI Send Duration And Frame Identity Diagnostics

**Files:**
- Modify: `playback/output/ndisink.h`
- Modify: `playback/output/ndisink.cpp`
- Test: `tests/unit/tst_ndisink.cpp`

- [ ] **Step 1: Write failing NDI status test**

In `FakeNdiBackend`, add configurable blocking:

```cpp
int sendDelayMs = 0;
```

Update `sendFrame()`:

```cpp
if (sendDelayMs > 0) QThread::msleep(uint(sendDelayMs));
```

Add test slot:

```cpp
void reportsSendDurationInOutputStatus();
```

Add body:

```cpp
void TestNdiSink::reportsSendDurationInOutputStatus() {
    FakeNdiBackend backend(true);
    backend.sendDelayMs = 5;
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));

    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = 42;
    frame.sampledPlayheadMs = 1680;
    frame.video = video(0, 1680, 88);
    frame.audio.feedIndex = 0;
    frame.audio.sampleRate = 48000;
    frame.audio.channels = 2;
    frame.audio.format = MediaSampleFormat::S16Interleaved;
    frame.audio.pcm = QByteArray(8 * int(sizeof(qint16)), '\0');

    QVERIFY(sink.submit(frame));
    const OutputSinkStatus status = sink.outputStatus();
    QVERIFY(status.lastSubmitDurationNs > 0);
    QVERIFY(status.hasLastDeliveredFrameIndex);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(42));
}
```

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
cmake --build build --target tst_ndisink
ctest --test-dir build -R '^tst_ndisink$' --output-on-failure
```

Expected: test fails because `lastSubmitDurationNs` and delivered frame index are not populated by `NdiOutputSink`.

- [ ] **Step 3: Extend NDI status**

In `NdiOutputStatus`, add:

```cpp
qint64 lastSendDurationNs = 0;
bool hasLastFrameIdentity = false;
```

Reset in `NdiOutputSink::start()`:

```cpp
m_status.lastSendDurationNs = 0;
m_status.hasLastFrameIdentity = false;
```

- [ ] **Step 4: Measure send duration**

In `ndisink.cpp`, include:

```cpp
#include <QElapsedTimer>
```

In `NdiOutputSink::submit()`, start a timer before validation and sending:

```cpp
QElapsedTimer sendTimer;
sendTimer.start();
```

When storing last frame identity:

```cpp
m_status.hasLastFrameIdentity = true;
```

Before each failure return after validation/send attempt, store:

```cpp
{
    QMutexLocker locker(&m_statusMutex);
    m_status.lastSendDurationNs = sendTimer.nsecsElapsed();
}
```

On success, store:

```cpp
m_status.lastSendDurationNs = sendTimer.nsecsElapsed();
```

- [ ] **Step 5: Map NDI status to generic sink status**

In `NdiOutputSink::outputStatus()`, add:

```cpp
out.lastSubmitDurationNs = ndi.lastSendDurationNs;
if (ndi.hasLastFrameIdentity) {
    out.hasLastQueuedFrameIndex = true;
    out.hasLastDeliveredFrameIndex = ndi.state == NdiOutputState::Active;
    out.lastQueuedFrameIndex = ndi.lastFrameIdentity.outputFrameIndex;
    if (out.hasLastDeliveredFrameIndex)
        out.lastDeliveredFrameIndex = ndi.lastFrameIdentity.outputFrameIndex;
}
```

- [ ] **Step 6: Run tests to verify GREEN**

Run:

```bash
cmake --build build --target tst_ndisink
ctest --test-dir build -R '^tst_ndisink$' --output-on-failure
```

Expected: `tst_ndisink` passes.

- [ ] **Step 7: Commit**

```bash
git add playback/output/ndisink.h playback/output/ndisink.cpp tests/unit/tst_ndisink.cpp
git commit -m "report ndi send duration"
```

---

### Task 5: Update NDI Output UI Diagnostics

**Files:**
- Modify: `Main.qml`
- Test: `qml_smoke`

- [ ] **Step 1: Update health text binding**

In the NDI output row `Health` label in `Main.qml`, replace the existing text with:

```qml
text: "F " + (ndiOutputRow.statusData.sinkFailures || 0)
      + " SF " + (ndiOutputRow.statusData.sinkFailedFrames || 0)
      + " D " + (ndiOutputRow.statusData.sinkDroppedFrames || 0)
      + " Q " + (ndiOutputRow.statusData.currentQueueDepth || 0)
      + "/" + (ndiOutputRow.statusData.maxQueueDepth || 0)
      + " G " + (ndiOutputRow.statusData.deliveryGaps || 0)
```

- [ ] **Step 2: Keep the tooltip as the full diagnostic**

Confirm the `ToolTip.text` for status, frames, health, and last labels remains:

```qml
ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
```

- [ ] **Step 3: Run QML smoke**

Run:

```bash
cmake --build build --target OpenLiveReplay
ctest --test-dir build -R '^qml_smoke$' --output-on-failure
```

Expected: build and `qml_smoke` pass.

- [ ] **Step 4: Commit**

```bash
git add Main.qml
git commit -m "show output queue health in ui"
```

---

### Task 6: PR 1 Verification And PR Body

**Files:**
- No source edits expected.

- [ ] **Step 1: Run focused output verification**

Run:

```bash
cmake --build build --target OpenLiveReplay tst_outputruntime tst_outputdispatcher tst_queuedoutputsink tst_ndisink tst_broadcastoutputsettings
ctest --test-dir build -R '^(tst_outputruntime|tst_outputdispatcher|tst_queuedoutputsink|tst_ndisink|tst_broadcastoutputsettings|qml_smoke)$' --output-on-failure
```

Expected: all listed tests pass.

- [ ] **Step 2: Run unit suite**

Run:

```bash
ctest --test-dir build -L unit --output-on-failure
```

Expected: all unit tests pass.

- [ ] **Step 3: Run formatting checks**

Run:

```bash
git diff --check
CF=/opt/homebrew/opt/llvm/bin/clang-format
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
BASE=$(git merge-base origin/main HEAD)
"$GCF" --binary "$CF" --commit "$BASE" --diff --extensions cpp,h,hpp,mm,c
```

Expected: `git diff --check` has no output. `git-clang-format` prints `clang-format did not modify any files` or `no modified files to format`.

- [ ] **Step 4: Update PR body**

Use this PR summary:

```markdown
## Summary
- Add output runtime deadline/catch-up telemetry.
- Report queued sink backpressure and delivery frame-index gaps.
- Surface deadline, queue, gap, and NDI send-duration diagnostics in broadcast output status.
- Keep output frame production unchanged while making timing failures visible.

## Validation
- cmake --build build --target OpenLiveReplay tst_outputruntime tst_outputdispatcher tst_queuedoutputsink tst_ndisink tst_broadcastoutputsettings
- ctest --test-dir build -R '^(tst_outputruntime|tst_outputdispatcher|tst_queuedoutputsink|tst_ndisink|tst_broadcastoutputsettings|qml_smoke)$' --output-on-failure
- ctest --test-dir build -L unit --output-on-failure
- git diff --check
- git-clang-format changed-lines check

## Notes
This PR does not add DeckLink/ST 2110 backends. It hardens the shared app-generated output timeline that those backends will consume.
```

---

## Later PR Plans

### PR 2: NDI Sender Hardening

After PR 1 lands, write a separate implementation plan for:

- NDI runtime version/path/status reporting.
- NDI frame format validation before sender start.
- Distinct NDI states for invalid video, invalid audio, runtime unavailable, sender create failure, send failure, and recovered active send.
- Receiver-backed runtime smoke that validates output frame indexes over configurable duration.
- 5-minute local soak command:

```bash
OLR_RUN_NDI_RUNTIME_TESTS=1 OLR_NDI_RUNTIME_SOAK_SECONDS=300 ctest --test-dir build -L ndi-runtime --output-on-failure
```

### PR 3: Rational Playback Sampling

Write a separate implementation plan for:

- Replacing rounded integer playback stepping with `FrameRate` rational math.
- Output sampling tests for 25, 30, 30000/1001, 50, and 60000/1001.
- Long-duration drift tests proving step/jog/shuttle playhead sampling remains frame-accurate.
- PGM feed switch tests proving identity changes on the next output tick without output clock reset.

### PR 4: Operator Health Model And Confidence Harness

Write a separate implementation plan for:

- Consolidating output health into a small helper instead of string logic inside `broadcastoutputsettings.cpp`.
- Adding long-run output continuity tests that assert no output index gaps under pause, play, reverse, PGM switch, and slow sink pressure.
- Making UI status vocabulary match broadcast operations: Active, Degraded, Error, Waiting, Off.

### PR 5+: Hardware/IP Broadcast Outputs

Only after PRs 1-4 establish the shared timing contract, write backend-specific plans for:

- DeckLink SDI/HDMI output.
- DeckLink IP / SMPTE ST 2110 output.
- AJA card output.
- OMT output.

Each backend plan must start from the same `OutputBusFrame` contract and must report the same deadline/backpressure/delivery health fields.

## Self-Review

- Spec coverage: the plan covers output cadence, queue backpressure, NDI send-duration diagnostics, UI surfacing, tests, and the later PR breakdown. It explicitly excludes recorder/input and hardware backend implementation from PR 1.
- Completion scan: no placeholder markers or open-ended implementation instructions remain.
- Type consistency: new fields are introduced first in `OutputRuntimeDispatchStats`, `OutputSinkStatus`, `OutputTargetDispatchStats`, and `BroadcastOutputTargetStatus`, then reused consistently in status mapping and UI fingerprinting.
- Scope check: PR 1 is independently testable and mergeable. Later NDI/playback/hardware work is intentionally split into follow-up plans.
