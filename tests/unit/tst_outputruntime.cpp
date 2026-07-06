#include <QtTest>

#include "playback/gpu/gpurhicontext.h"
#include "playback/output/outputruntime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

static FrameHandle video(int feed, qint64 pts, uchar y) {
    FrameHandle f = solidYuv420pHandle(4, 4, y, 128, 128);
    f.metadata().key.feedIndex = feed;
    f.metadata().key.ptsMs = pts;
    return f;
}

static uchar yAt(const OutputBusFrame& frame, qsizetype offset) {
    return uchar(MediaVideoFrameView(frame.video).planeY.at(offset));
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

class RuntimeSetterDuringSubmitSink final : public IOutputSink {
public:
    ~RuntimeSetterDuringSubmitSink() override { joinSetter(); }

    void setRuntime(OutputRuntime* runtime) { m_runtime = runtime; }

    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }

    void stop() override { m_active = false; }

    bool isActive() const override { return m_active; }

    bool submit(const OutputBusFrame&) override {
        if (!m_active || !m_runtime) return false;

        joinSetter();
        m_setterStarted.store(false, std::memory_order_release);
        m_setterReturned.store(false, std::memory_order_release);
        m_setterReturnedDuringSubmit.store(false, std::memory_order_release);
        m_setterThread = std::thread([this]() {
            m_setterStarted.store(true, std::memory_order_release);
            m_runtime->setSnapshotProvider([]() { return OutputRuntimeSnapshot(); });
            m_setterReturned.store(true, std::memory_order_release);
            m_setterReturnedCv.notify_all();
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!m_setterStarted.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::unique_lock<std::mutex> lock(m_setterReturnedMutex);
        const bool returned = m_setterReturnedCv.wait_for(lock, std::chrono::seconds(2), [this]() {
            return m_setterReturned.load(std::memory_order_acquire);
        });
        m_setterReturnedDuringSubmit.store(returned, std::memory_order_release);
        return true;
    }

    bool setterReturnedDuringSubmit() const {
        return m_setterReturnedDuringSubmit.load(std::memory_order_acquire);
    }

    void joinSetter() {
        if (m_setterThread.joinable()) m_setterThread.join();
    }

private:
    OutputRuntime* m_runtime = nullptr;
    bool m_active = false;
    std::atomic<bool> m_setterStarted = false;
    std::atomic<bool> m_setterReturned = false;
    std::atomic<bool> m_setterReturnedDuringSubmit = false;
    std::mutex m_setterReturnedMutex;
    std::condition_variable m_setterReturnedCv;
    std::thread m_setterThread;
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
    void fenceWaitStallsCanBeIncremented();
    void recordGpuBudgetSurfacesInStats();
    void injectedGpuRhiContextIsReusedAndReplaceable();
    void dispatchSubmitsWithoutHoldingRuntimeMutex();
    void endpointReconfigurationDiscardsPreReconfigSnapshot();
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
    // This test asserts a per-tick submit of the SAME paused frame; identity-skip
    // (default on) would collapse the repeats, so disable it here.
    runtime.setIdentitySkip(false);

    runtime.dispatchDueTicksForTest(0);
    runtime.dispatchDueTicksForTest(40);
    runtime.dispatchDueTicksForTest(80);

    const QVector<OutputBusFrame> frames = sink.frames();
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(frames[2].outputFrameIndex, qint64(2));
    QCOMPARE(frames[0].video.metadata().key.ptsMs, qint64(100));
    QCOMPARE(frames[2].video.metadata().key.ptsMs, qint64(100));
    QCOMPARE(yAt(frames[2], 0), uchar(40));
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
    // The runtime thread re-ticks the SAME paused frame; identity-skip (default on)
    // would collapse the repeats to one submit, so disable it for this assertion.
    runtime.setIdentitySkip(false);

    runtime.startRuntime();
    QTRY_VERIFY_WITH_TIMEOUT(sink.frameCount() >= 3, 500);
    runtime.stopRuntime();

    const QVector<OutputBusFrame> frames = sink.frames();
    QVERIFY(frames.size() >= 3);
    QCOMPARE(frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(frames[2].outputFrameIndex, qint64(2));
    QCOMPARE(yAt(frames[2], 0), uchar(55));
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

void TestOutputRuntime::fenceWaitStallsCanBeIncremented() {
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);

    QCOMPARE(runtime.stats().fenceWaitStalls, qint64(0));
    runtime.incrementFenceWaitStalls();
    runtime.incrementFenceWaitStalls();

    QCOMPARE(runtime.stats().fenceWaitStalls, qint64(2));
}

void TestOutputRuntime::recordGpuBudgetSurfacesInStats() {
    OutputRuntime runtime(FrameRate::fromFraction(60, 1), 1, 64, 48);

    QCOMPARE(runtime.stats().gpuVramBytes, qint64(0));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(0));
    QCOMPARE(runtime.stats().gpuDeviceLossEvents, qint64(0));

    runtime.recordGpuBudget(3110400, 2);
    runtime.recordGpuDeviceLossEvents(1);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(3110400));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(2));
    QCOMPARE(runtime.stats().gpuDeviceLossEvents, qint64(1));

    runtime.recordGpuBudget(6220800, 3);
    runtime.recordGpuDeviceLossEvents(2);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(6220800));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(3));
    QCOMPARE(runtime.stats().gpuDeviceLossEvents, qint64(2));

    runtime.recordGpuDeviceLossEvents(-1);
    QCOMPARE(runtime.stats().gpuDeviceLossEvents, qint64(0));
}

void TestOutputRuntime::injectedGpuRhiContextIsReusedAndReplaceable() {
#ifndef OLR_GPU_PIPELINE_BUILD
    QSKIP("GPU pipeline disabled");
#else
    qputenv("OLR_GPU_PIPELINE", "1");
    auto initial = GpuRhiContext::createInvalidForTest();
    auto replacement = GpuRhiContext::createInvalidForTest();
    QVERIFY(initial != nullptr);
    QVERIFY(replacement != nullptr);
    QVERIFY(initial != replacement);

    OutputRuntime runtime(FrameRate::fromFraction(60, 1), 1, 64, 48, initial);
    QCOMPARE(runtime.gpuRhiContextForTest(), initial);

    runtime.setGpuRhiContext(replacement);

    QCOMPARE(runtime.gpuRhiContextForTest(), replacement);
    qunsetenv("OLR_GPU_PIPELINE");
#endif
}

void TestOutputRuntime::dispatchSubmitsWithoutHoldingRuntimeMutex() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 105));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    RuntimeSetterDuringSubmitSink sink;
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    sink.setRuntime(&runtime);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    runtime.dispatchDueTicksForTest(0);
    sink.joinSetter();

    QVERIFY2(sink.setterReturnedDuringSubmit(),
             "sink submission must not run while OutputRuntime::m_mutex is held; GPU readback "
             "sinks can block on fences while submitting");
}

void TestOutputRuntime::endpointReconfigurationDiscardsPreReconfigSnapshot() {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    ThreadSafeCollectingSink sink(OutputTargetKind::QtPreview);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);

    OutputFrameCache full(1, 4, 4);
    full.insertVideoFrame(video(0, 100, 91));
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    int snapshots = 0;
    runtime.setSnapshotProvider([&]() {
        ++snapshots;
        OutputRuntimeSnapshot snapshot;
        snapshot.state = state;
        if (snapshots == 1) {
            runtime.setEndpoints({{assignment, &sink}});
            return snapshot;
        }
        snapshot.cache = full;
        return snapshot;
    });

    runtime.dispatchDueTicksForTest(0);

    QCOMPARE(snapshots, 2);
    const QVector<OutputBusFrame> frames = sink.frames();
    QCOMPARE(frames.size(), 1);
    QVERIFY(!frames.first().video.metadata().key.isPlaceholder);
    QCOMPARE(frames.first().video.metadata().key.ptsMs, qint64(100));
    QCOMPARE(yAt(frames.first(), 0), uchar(91));
}

QTEST_GUILESS_MAIN(TestOutputRuntime)
#include "tst_outputruntime.moc"
