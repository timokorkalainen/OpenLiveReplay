#include <QtTest>

#include "playback/output/outputruntime.h"

static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = pts;
    return f;
}

class ThreadSafeCollectingSink final : public IOutputSink {
public:
    explicit ThreadSafeCollectingSink(OutputTargetKind kind) : m_kind(kind) {}

    OutputTargetKind kind() const override { return m_kind; }

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
        m_frames.append(frame);
        return true;
    }

    int frameCount() const {
        QMutexLocker locker(&m_mutex);
        return m_frames.size();
    }

    QVector<OutputBusFrame> frames() const {
        QMutexLocker locker(&m_mutex);
        return m_frames;
    }

private:
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    mutable QMutex m_mutex;
    bool m_active = false;
    QVector<OutputBusFrame> m_frames;
};

class TestOutputRuntime : public QObject {
    Q_OBJECT
private slots:
    void manualTicksRepeatPausedFrameFromCache();
    void nanosecondTicksHonorFractionalFrameBoundary();
    void workerThreadTicksWithoutExternalDispatchCalls();
    void runtimeStatsReportNoDeadlineMissForOnTimeTicks();
    void exactlyMaxCatchUpTicksDoesNotReportCapHit();
    void runtimeStatsReportDeadlineMissWhenCatchUpIsCapped();
    void runtimeClearsDeadlineMissLatchAfterRecovery();
};

void TestOutputRuntime::manualTicksRepeatPausedFrameFromCache() {
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

    runtime.dispatchDueTicksForTest(0);
    runtime.dispatchDueTicksForTest(40);
    runtime.dispatchDueTicksForTest(80);

    const QVector<OutputBusFrame> frames = sink.frames();
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(frames[2].outputFrameIndex, qint64(2));
    QCOMPARE(frames[0].video.ptsMs, qint64(100));
    QCOMPARE(frames[2].video.ptsMs, qint64(100));
    QCOMPARE(uchar(frames[2].video.planeY.at(0)), uchar(40));
}

void TestOutputRuntime::nanosecondTicksHonorFractionalFrameBoundary() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 0, 70));

    PlaybackStateSnapshot state;
    state.playheadMs = 0;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    ThreadSafeCollectingSink sink(OutputTargetKind::QtPreview);
    OutputRuntime runtime(FrameRate::fromFraction(30000, 1001), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    runtime.dispatchDueTicksForTestNs(0);
    runtime.dispatchDueTicksForTestNs(33366666);
    QCOMPARE(sink.frameCount(), 1);

    runtime.dispatchDueTicksForTestNs(33366667);
    QCOMPARE(sink.frameCount(), 2);
    QCOMPARE(sink.frames()[1].outputFrameIndex, qint64(1));
}

void TestOutputRuntime::workerThreadTicksWithoutExternalDispatchCalls() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 55));

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
    OutputRuntime runtime(FrameRate::fromFraction(50, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    runtime.startRuntime();
    QTRY_VERIFY_WITH_TIMEOUT(sink.frameCount() >= 3, 500);
    runtime.stopRuntime();

    const QVector<OutputBusFrame> frames = sink.frames();
    QVERIFY(frames.size() >= 3);
    QCOMPARE(frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(frames[2].outputFrameIndex, qint64(2));
    QCOMPARE(uchar(frames[2].video.planeY.at(0)), uchar(55));
    QCOMPARE(runtime.stats().runtime.deadlineMisses, qint64(0));
}

void TestOutputRuntime::runtimeStatsReportNoDeadlineMissForOnTimeTicks() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 80));

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

    runtime.dispatchDueTicksForTest(0);
    runtime.dispatchDueTicksForTest(40);
    const OutputDispatchStats stats = runtime.dispatchDueTicksForTest(80);

    QCOMPARE(stats.ticks, qint64(3));
    QCOMPARE(stats.runtime.deadlineMisses, qint64(0));
    QCOMPARE(stats.runtime.catchUpCapHits, qint64(0));
    QCOMPARE(stats.runtime.lastDispatchedFrameIndex, qint64(2));
    QVERIFY(stats.runtime.hasLastDispatchTiming);
    QVERIFY(stats.runtime.lastLatenessNs <= 0);
}

void TestOutputRuntime::exactlyMaxCatchUpTicksDoesNotReportCapHit() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 85));

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

    runtime.dispatchDueTicksForTest(0);
    const OutputDispatchStats stats = runtime.dispatchDueTicksForTest(320);

    QCOMPARE(stats.ticks, qint64(9));
    QCOMPARE(stats.runtime.deadlineMisses, qint64(0));
    QCOMPARE(stats.runtime.catchUpCapHits, qint64(0));
    QCOMPARE(stats.runtime.cappedCatchUpTicks, qint64(0));
    QVERIFY(!stats.runtime.lastDispatchDeadlineMiss);
}

void TestOutputRuntime::runtimeStatsReportDeadlineMissWhenCatchUpIsCapped() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 90));

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

    runtime.dispatchDueTicksForTest(0);
    const OutputDispatchStats stats = runtime.dispatchDueTicksForTest(600);

    QCOMPARE(stats.ticks, qint64(9));
    QVERIFY(stats.runtime.deadlineMisses > 0);
    QVERIFY(stats.runtime.catchUpCapHits > 0);
    QCOMPARE(stats.runtime.cappedCatchUpTicks, qint64(7));
    QVERIFY(stats.runtime.lastDispatchDeadlineMiss);
    QCOMPARE(stats.runtime.lastCappedCatchUpTicks, qint64(7));
    QVERIFY(stats.runtime.maxLatenessNs > 0);
    QCOMPARE(stats.runtime.lastDispatchedFrameIndex, qint64(8));
}

void TestOutputRuntime::runtimeClearsDeadlineMissLatchAfterRecovery() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 95));

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

    runtime.dispatchDueTicksForTest(0);
    const OutputDispatchStats capped = runtime.dispatchDueTicksForTest(600);
    QVERIFY(capped.runtime.lastDispatchDeadlineMiss); // missed cadence: catch-up was capped

    // Drain the backlog on subsequent on-time polls; the per-poll latch must clear while
    // the cumulative miss counters persist for diagnostics.
    const OutputDispatchStats recovered = runtime.dispatchDueTicksForTest(640);
    QVERIFY2(!recovered.runtime.lastDispatchDeadlineMiss,
             "the deadline-miss latch must clear once the runtime catches up");
    QCOMPARE(recovered.runtime.lastCappedCatchUpTicks, qint64(0));
    QVERIFY(recovered.runtime.deadlineMisses > 0); // cumulative history is retained

    // A subsequent poll with no frame due must continue to report no current miss.
    const OutputDispatchStats idle = runtime.dispatchDueTicksForTest(645);
    QVERIFY(!idle.runtime.lastDispatchDeadlineMiss);
}

QTEST_GUILESS_MAIN(TestOutputRuntime)
#include "tst_outputruntime.moc"
