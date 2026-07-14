#include <QtTest>

#include "playback/gpu/gpubudget.h"
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

static FrameHandle videoWithGeneration(int feed, qint64 pts, uchar y, uint64_t gpuGeneration,
                                       qint64 decodedSequence) {
    FrameHandle frame = video(feed, pts, y);
    frame.metadata().gpuGeneration = gpuGeneration;
    frame.metadata().decodedSequence = decodedSequence;
    return frame;
}

static uchar yAt(const OutputBusFrame& frame, qsizetype offset) {
    return uchar(MediaVideoFrameView(frame.video).planeY.at(offset));
}

static qint64 videoPts(const OutputBusFrame& frame) {
    return frame.video.metadata().key.ptsMs;
}

class ThreadSafeCollectingSink final : public IOutputSink {
public:
    explicit ThreadSafeCollectingSink(OutputTargetKind kind, bool continuousCadence = false)
        : m_kind(kind), m_continuousCadence(continuousCadence) {}

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

    bool needsContinuousCadence() const override { return m_continuousCadence; }

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
    bool m_continuousCadence = false;
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

class RuntimeResetDuringSubmitSink final : public IOutputSink {
public:
    explicit RuntimeResetDuringSubmitSink(int blockedSubmit = 1) : m_blockedSubmit(blockedSubmit) {}
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }
    void stop() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_release = true;
        m_releaseCv.notify_all();
    }
    bool isActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_active;
    }
    bool submit(const OutputBusFrame& frame) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_active) return false;
        m_frames.append(frame);
        ++m_submitCount;
        if (m_submitCount != m_blockedSubmit) return true;

        m_entered = true;
        m_enteredCv.notify_all();
        if (!m_releaseCv.wait_for(lock, std::chrono::seconds(2), [this]() { return m_release; })) {
            m_diagnosticTimeout = true;
            return false;
        }
        m_resetReturnedBeforeRelease = m_resetReturned;
        return true;
    }

    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_enteredCv.wait_for(lock, std::chrono::seconds(2), [this]() { return m_entered; });
    }
    void markResetReturned() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_resetReturned = true;
        m_resetReturnedCv.notify_all();
    }
    bool waitForResetReturned() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_resetReturnedCv.wait_for(lock, std::chrono::seconds(2),
                                          [this]() { return m_resetReturned; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_release = true;
        m_releaseCv.notify_all();
    }
    bool resetReturnedBeforeRelease() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_resetReturnedBeforeRelease;
    }
    bool diagnosticTimeout() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_diagnosticTimeout;
    }
    QVector<OutputBusFrame> frames() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frames;
    }

private:
    const int m_blockedSubmit = 1;
    mutable std::mutex m_mutex;
    std::condition_variable m_enteredCv;
    std::condition_variable m_releaseCv;
    std::condition_variable m_resetReturnedCv;
    bool m_active = false;
    int m_submitCount = 0;
    bool m_entered = false;
    bool m_release = false;
    bool m_resetReturned = false;
    bool m_resetReturnedBeforeRelease = false;
    bool m_diagnosticTimeout = false;
    QVector<OutputBusFrame> m_frames;
};

struct DeferredResetScenarioResult {
    bool activeLeaseEntered = false;
    bool resetsReturnedBeforeRelease = false;
    bool followerStartedBeforeRelease = false;
    bool sinkObservedResetReturnBeforeRelease = false;
    bool diagnosticTimeout = false;
    int appliedResetsBeforeRelease = -1;
    int appliedResetsAtFollowerSnapshot = -1;
    int finalAppliedResets = -1;
    QVector<OutputBusFrame> frames;
};

static DeferredResetScenarioResult runDeferredResetScenario(bool holdLastActiveLease) {
    DeferredResetScenarioResult result;

    OutputFrameCache initialCache(1, 4, 4);
    initialCache.insertVideoFrame(videoWithGeneration(0, 100, 41, 7, 701));
    OutputFrameCache activeCache(1, 4, 4);
    if (!holdLastActiveLease) activeCache.insertVideoFrame(videoWithGeneration(0, 140, 82, 7, 702));
    OutputFrameCache followerCache(1, 4, 4);
    followerCache.insertVideoFrame(videoWithGeneration(0, 500, 123, 8, 801));

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    RuntimeResetDuringSubmitSink sink(2);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    std::atomic<int> snapshotStage{0};
    std::atomic<int> appliedResetsAtFollowerSnapshot{-1};
    runtime.setSnapshotProvider([&]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.state.playing = true;
        snapshot.state.selectedFeedIndex = 0;
        const int stage = snapshotStage.load(std::memory_order_acquire);
        if (stage == 0) {
            snapshot.cache = initialCache;
            snapshot.state.playheadMs = 100;
            snapshot.state.gpuGeneration = 7;
        } else if (stage == 1) {
            snapshot.cache = activeCache;
            snapshot.state.playheadMs = 140;
            snapshot.state.gpuGeneration = 7;
        } else {
            appliedResetsAtFollowerSnapshot.store(runtime.playEpochResetCountForTest(),
                                                  std::memory_order_release);
            snapshot.cache = followerCache;
            snapshot.state.playheadMs = 500;
            snapshot.state.gpuGeneration = 8;
        }
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});
    runtime.setIdentitySkip(false);

    runtime.dispatchImmediate();
    snapshotStage.store(1, std::memory_order_release);
    std::thread activeDispatch([&]() { runtime.dispatchImmediate(); });
    result.activeLeaseEntered = sink.waitUntilEntered();

    std::thread resetter([&]() {
        runtime.resetPlayEpoch();
        runtime.resetPlayEpoch();
        runtime.resetPlayEpoch();
        sink.markResetReturned();
    });
    result.resetsReturnedBeforeRelease = sink.waitForResetReturned();
    if (result.resetsReturnedBeforeRelease)
        result.appliedResetsBeforeRelease = runtime.playEpochResetCountForTest();

    snapshotStage.store(2, std::memory_order_release);
    std::mutex followerMutex;
    std::condition_variable followerStartedCv;
    bool followerStarted = false;
    std::thread followerDispatch([&]() {
        {
            std::lock_guard<std::mutex> lock(followerMutex);
            followerStarted = true;
            followerStartedCv.notify_all();
        }
        runtime.dispatchImmediate();
    });
    {
        std::unique_lock<std::mutex> lock(followerMutex);
        result.followerStartedBeforeRelease = followerStartedCv.wait_for(
            lock, std::chrono::seconds(2), [&]() { return followerStarted; });
    }

    sink.release();
    activeDispatch.join();
    resetter.join();
    followerDispatch.join();

    result.sinkObservedResetReturnBeforeRelease = sink.resetReturnedBeforeRelease();
    result.diagnosticTimeout = sink.diagnosticTimeout();
    result.appliedResetsAtFollowerSnapshot =
        appliedResetsAtFollowerSnapshot.load(std::memory_order_acquire);
    result.finalAppliedResets = runtime.playEpochResetCountForTest();
    result.frames = sink.frames();
    return result;
}

class TestOutputRuntime : public QObject {
    Q_OBJECT
private slots:
    void pausedScheduledTicksAdvanceClockWithoutPreviewSubmit();
    void nanosecondTicksHonorFractionalFrameBoundary();
    void workerThreadIdlesWhilePausedWithoutExternalDispatchCalls();
    void pausedScheduledTicksKeepPgmExternalCadenceWithoutPreviewSubmit();
    void pausedPgmCadenceTicksLeavePgmCriticalImmediateDispatchCommandOwned();
    void pausedClockOnlyTicksDoNotCreateCatchUpBurstOnResume();
    void runtimeStatsReportNoDeadlineMissForOnTimeTicks();
    void exactlyMaxCatchUpTicksDoesNotReportCapHit();
    void runtimeStatsReportDeadlineMissWhenCatchUpIsCapped();
    void runtimeClearsDeadlineMissLatchAfterRecovery();
    void fenceWaitStallsCanBeIncremented();
    void recordGpuBudgetSurfacesInStats();
    void injectedGpuRhiContextIsReusedAndReplaceable();
    void dispatchSubmitsWithoutHoldingRuntimeMutex();
    void resetRejectsSnapshotCapturedBeforeLeaseRecheck();
    void playEpochResetDefersWithoutBlockingActiveDispatch();
    void multiplePlayEpochResetsCoalesceBeforeNextLease();
    void multiplePlayEpochResetsCoalesceBeforeNextHoldLastLease();
    void immediateDispatchPreemptsCatchUpBurstAfterCurrentTick();
    void pgmCriticalImmediateDispatchSubmitsPreviewAndReportsPgmIdentity();
    void endpointReconfigurationDiscardsPreReconfigSnapshot();
};

void TestOutputRuntime::pausedScheduledTicksAdvanceClockWithoutPreviewSubmit() {
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
    // Even with identity-skip disabled, the runtime's background clock must not
    // churn the same paused preview frame. Operator-owned immediate dispatches
    // update paused previews and external outputs.
    runtime.setIdentitySkip(false);

    runtime.dispatchDueTicksForTest(0);
    runtime.dispatchDueTicksForTest(40);
    const OutputDispatchStats stats = runtime.dispatchDueTicksForTest(80);

    QCOMPARE(sink.frameCount(), 0);
    QCOMPARE(stats.ticks, qint64(3));
    QCOMPARE(stats.framesSubmitted, qint64(0));
    QCOMPARE(stats.runtime.lastDispatchedFrameIndex, qint64(2));
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(3));
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
    QCOMPARE(sink.frameCount(), 0);
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(1));

    runtime.dispatchDueTicksForTestNs(33366667);
    QCOMPARE(sink.frameCount(), 0);
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(2));
}

void TestOutputRuntime::workerThreadIdlesWhilePausedWithoutExternalDispatchCalls() {
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
    // Disable identity-skip to prove the runtime itself suppresses paused
    // background submits, not just the dispatcher duplicate filter.
    runtime.setIdentitySkip(false);

    runtime.startRuntime();
    QTest::qWait(160);
    runtime.stopRuntime();

    QCOMPARE(sink.frameCount(), 0);
    QVERIFY(runtime.stats().ticks >= 3);
    QCOMPARE(runtime.stats().runtime.deadlineMisses, qint64(0));
}

void TestOutputRuntime::pausedScheduledTicksKeepPgmExternalCadenceWithoutPreviewSubmit() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 1000, 88));

    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment preview;
    preview.id = QStringLiteral("pgm-preview");
    preview.sourceBus = OutputBusId::pgm();
    preview.kind = OutputTargetKind::QtPreview;
    preview.enabled = true;

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    ThreadSafeCollectingSink previewSink(OutputTargetKind::QtPreview);
    ThreadSafeCollectingSink pgmSink(OutputTargetKind::Ndi, true);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{preview, &previewSink}, {pgm, &pgmSink}});

    runtime.dispatchDueTicksForTest(0);
    runtime.dispatchDueTicksForTest(40);
    const OutputDispatchStats stats = runtime.dispatchDueTicksForTest(80);

    QCOMPARE(previewSink.frameCount(), 0);
    QCOMPARE(pgmSink.frameCount(), 3);
    QCOMPARE(stats.ticks, qint64(3));
    QCOMPARE(stats.framesSubmitted, qint64(3));
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(3));

    const QVector<OutputBusFrame> frames = pgmSink.frames();
    QCOMPARE(frames.size(), 3);
    for (int i = 0; i < frames.size(); ++i) {
        QCOMPARE(frames.at(i).bus, OutputBusId::pgm());
        QCOMPARE(frames.at(i).outputFrameIndex, qint64(i));
        QCOMPARE(frames.at(i).sampledPlayheadMs, qint64(1000));
        QCOMPARE(videoPts(frames.at(i)), qint64(1000));
        QVERIFY(!frames.at(i).identity.videoPlaceholder);
    }
}

void TestOutputRuntime::pausedPgmCadenceTicksLeavePgmCriticalImmediateDispatchCommandOwned() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 1000, 88));

    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment preview;
    preview.id = QStringLiteral("pgm-preview");
    preview.sourceBus = OutputBusId::pgm();
    preview.kind = OutputTargetKind::QtPreview;
    preview.enabled = true;

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    ThreadSafeCollectingSink previewSink(OutputTargetKind::QtPreview);
    ThreadSafeCollectingSink pgmSink(OutputTargetKind::Ndi, true);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });
    runtime.setEndpoints({{preview, &previewSink}, {pgm, &pgmSink}});

    runtime.dispatchDueTicksForTest(0);
    runtime.dispatchDueTicksForTest(40);
    runtime.dispatchDueTicksForTest(80);
    QCOMPARE(previewSink.frameCount(), 0);
    QCOMPARE(pgmSink.frameCount(), 3);
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(3));

    OutputDispatchRequest request;
    request.lane = OutputDispatchLane::PgmCritical;
    request.requiredBus = OutputBusId::pgm();
    request.requiredKind = OutputTargetKind::Ndi;
    request.requiredPlayheadMs = 1000;
    request.requireNonPlaceholder = true;

    const OutputDispatchReport report = runtime.dispatchImmediateWithReport(request);

    QVERIFY(report.requiredSubmitted);
    QCOMPARE(report.requiredIdentity.bus, OutputBusId::pgm());
    QCOMPARE(report.requiredIdentity.sourcePtsMs, qint64(1000));
    QCOMPARE(pgmSink.frameCount(), 4);
    QCOMPARE(previewSink.frameCount(), 1);
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(4));
}

void TestOutputRuntime::pausedClockOnlyTicksDoNotCreateCatchUpBurstOnResume() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 45));

    std::atomic_bool playing{false};

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    ThreadSafeCollectingSink sink(OutputTargetKind::QtPreview);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([&]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state.playheadMs = 100;
        snapshot.state.playing = playing.load(std::memory_order_acquire);
        snapshot.state.selectedFeedIndex = 0;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});
    runtime.setIdentitySkip(false);

    runtime.dispatchDueTicksForTest(0);
    const OutputDispatchStats pausedStats = runtime.dispatchDueTicksForTest(320);
    QCOMPARE(sink.frameCount(), 0);
    QCOMPARE(pausedStats.ticks, qint64(9));
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(9));
    QVERIFY(!pausedStats.runtime.lastDispatchDeadlineMiss);

    playing.store(true, std::memory_order_release);
    const OutputDispatchStats resumedStats = runtime.dispatchDueTicksForTest(360);

    QCOMPARE(sink.frameCount(), 1);
    QCOMPARE(sink.frames().first().outputFrameIndex, qint64(9));
    QCOMPARE(runtime.dispatcherNextOutputFrameIndex(), qint64(10));
    QVERIFY(!resumedStats.runtime.lastDispatchDeadlineMiss);
    QCOMPARE(resumedStats.runtime.deadlineMisses, qint64(0));
    QCOMPARE(resumedStats.runtime.catchUpCapHits, qint64(0));
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
    QCOMPARE(runtime.stats().gpuBudgetBytes, qint64(0));
    QCOMPARE(runtime.stats().gpuGatedLiveBytes, qint64(0));
    QVERIFY(!runtime.stats().gpuBudgetReportOnly);
    for (qint64 taggedBytes : runtime.stats().gpuLiveBytesByTag)
        QCOMPARE(taggedBytes, qint64(0));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(0));
    QCOMPARE(runtime.stats().gpuDeviceLossEvents, qint64(0));

    GpuBudgetSnapshot snapshot;
    snapshot.budgetBytes = 9000000;
    snapshot.liveBytes = 3110400;
    snapshot.gatedLiveBytes = 2000000;
    snapshot.oomDegrades = 2;
    snapshot.reportOnly = true;
    snapshot.liveBytesByTag[static_cast<int>(GpuBudgetTag::DecodeWindow)] = 2000000;
    snapshot.liveBytesByTag[static_cast<int>(GpuBudgetTag::IngestWrap)] = 1110400;

    runtime.recordGpuBudget(snapshot);
    runtime.recordGpuDeviceLossEvents(1);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(3110400));
    QCOMPARE(runtime.stats().gpuBudgetBytes, qint64(9000000));
    QCOMPARE(runtime.stats().gpuGatedLiveBytes, qint64(2000000));
    QVERIFY(runtime.stats().gpuBudgetReportOnly);
    QCOMPARE(runtime.stats().gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::DecodeWindow)],
             qint64(2000000));
    QCOMPARE(runtime.stats().gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::IngestWrap)],
             qint64(1110400));
    QCOMPARE(runtime.stats().gpuOomDegrades, qint64(2));
    QCOMPARE(runtime.stats().gpuDeviceLossEvents, qint64(1));

    snapshot = {};
    snapshot.budgetBytes = 12000000;
    snapshot.liveBytes = 6220800;
    snapshot.gatedLiveBytes = 3000000;
    snapshot.oomDegrades = 3;
    snapshot.liveBytesByTag[static_cast<int>(GpuBudgetTag::OutputBus)] = 3000000;
    snapshot.liveBytesByTag[static_cast<int>(GpuBudgetTag::RecorderWrap)] = 3220800;
    runtime.recordGpuBudget(snapshot);
    runtime.recordGpuDeviceLossEvents(2);
    QCOMPARE(runtime.stats().gpuVramBytes, qint64(6220800));
    QCOMPARE(runtime.stats().gpuBudgetBytes, qint64(12000000));
    QCOMPARE(runtime.stats().gpuGatedLiveBytes, qint64(3000000));
    QVERIFY(!runtime.stats().gpuBudgetReportOnly);
    QCOMPARE(runtime.stats().gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::DecodeWindow)],
             qint64(0));
    QCOMPARE(runtime.stats().gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::OutputBus)],
             qint64(3000000));
    QCOMPARE(runtime.stats().gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::RecorderWrap)],
             qint64(3220800));
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
    state.playing = true;
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

void TestOutputRuntime::resetRejectsSnapshotCapturedBeforeLeaseRecheck() {
    OutputFrameCache staleCache(1, 4, 4);
    staleCache.insertVideoFrame(videoWithGeneration(0, 100, 35, 11, 1101));
    OutputFrameCache currentCache(1, 4, 4);
    currentCache.insertVideoFrame(videoWithGeneration(0, 500, 95, 12, 1201));

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    ThreadSafeCollectingSink sink(OutputTargetKind::QtPreview);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    std::atomic<int> snapshotCalls{0};
    std::mutex snapshotMutex;
    std::condition_variable snapshotCapturedCv;
    std::condition_variable snapshotReleaseCv;
    bool snapshotCaptured = false;
    bool releaseSnapshot = false;
    bool diagnosticTimeout = false;
    runtime.setSnapshotProvider([&]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.state.playing = true;
        snapshot.state.selectedFeedIndex = 0;
        const int call = snapshotCalls.fetch_add(1, std::memory_order_acq_rel);
        if (call == 0) {
            snapshot.cache = staleCache;
            snapshot.state.playheadMs = 100;
            snapshot.state.gpuGeneration = 11;
            std::unique_lock<std::mutex> lock(snapshotMutex);
            snapshotCaptured = true;
            snapshotCapturedCv.notify_all();
            if (!snapshotReleaseCv.wait_for(lock, std::chrono::seconds(2),
                                            [&]() { return releaseSnapshot; }))
                diagnosticTimeout = true;
        } else {
            snapshot.cache = currentCache;
            snapshot.state.playheadMs = 500;
            snapshot.state.gpuGeneration = 12;
        }
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    std::thread dispatch([&]() { runtime.dispatchDueTicksForTest(0); });
    bool capturedBeforeReset = false;
    {
        std::unique_lock<std::mutex> lock(snapshotMutex);
        capturedBeforeReset = snapshotCapturedCv.wait_for(lock, std::chrono::seconds(2),
                                                          [&]() { return snapshotCaptured; });
    }
    if (capturedBeforeReset) runtime.resetPlayEpoch();
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        releaseSnapshot = true;
        snapshotReleaseCv.notify_all();
    }
    dispatch.join();

    QVERIFY2(capturedBeforeReset, "snapshot provider did not reach the capture barrier");
    QVERIFY2(!diagnosticTimeout, "snapshot provider timed out waiting for deterministic release");
    QCOMPARE(snapshotCalls.load(std::memory_order_acquire), 2);
    const QVector<OutputBusFrame> frames = sink.frames();
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().outputFrameIndex, qint64(0));
    QCOMPARE(frames.first().sampledPlayheadMs, qint64(500));
    QCOMPARE(videoPts(frames.first()), qint64(500));
    QCOMPARE(frames.first().video.metadata().gpuGeneration, uint64_t(12));
    QCOMPARE(frames.first().video.metadata().decodedSequence, qint64(1201));
    QCOMPARE(yAt(frames.first(), 0), uchar(95));
}

void TestOutputRuntime::playEpochResetDefersWithoutBlockingActiveDispatch() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 105));

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    RuntimeResetDuringSubmitSink sink;
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state.playheadMs = 100;
        snapshot.state.playing = true;
        snapshot.state.selectedFeedIndex = 0;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});

    std::thread dispatch([&]() { runtime.dispatchDueTicksForTest(0); });
    const bool entered = sink.waitUntilEntered();
    std::thread resetter([&]() {
        runtime.resetPlayEpoch();
        sink.markResetReturned();
    });
    const bool resetReturned = sink.waitForResetReturned();
    sink.release();
    dispatch.join();
    resetter.join();

    QVERIFY2(entered, "sink submit did not reach the active-dispatch barrier");
    QVERIFY2(resetReturned, "resetPlayEpoch did not return before the release barrier");
    QVERIFY2(!sink.diagnosticTimeout(), "sink timed out waiting for deterministic release");
    QVERIFY2(sink.resetReturnedBeforeRelease(),
             "resetPlayEpoch must invalidate the active snapshot and defer the epoch clear "
             "without blocking the seek commit behind sink submission");
}

void TestOutputRuntime::multiplePlayEpochResetsCoalesceBeforeNextLease() {
    const DeferredResetScenarioResult result = runDeferredResetScenario(false);

    QVERIFY2(result.activeLeaseEntered, "active real-frame lease did not reach submit barrier");
    QVERIFY2(result.resetsReturnedBeforeRelease,
             "three resets did not return while the real-frame lease remained active");
    QVERIFY2(result.followerStartedBeforeRelease,
             "follower dispatch did not start before the active lease was released");
    QVERIFY2(result.sinkObservedResetReturnBeforeRelease,
             "sink did not observe reset completion before its release barrier");
    QVERIFY2(!result.diagnosticTimeout, "real-frame lease hit the diagnostic timeout");
    QCOMPARE(result.appliedResetsBeforeRelease, 0);
    QCOMPARE(result.appliedResetsAtFollowerSnapshot, 1);
    QCOMPARE(result.finalAppliedResets, 1);
    QCOMPARE(result.frames.size(), 3);

    const OutputBusFrame& active = result.frames.at(1);
    QCOMPARE(active.outputFrameIndex, qint64(1));
    QCOMPARE(active.sampledPlayheadMs, qint64(140));
    QCOMPARE(videoPts(active), qint64(140));
    QCOMPARE(active.video.metadata().gpuGeneration, uint64_t(7));
    QCOMPARE(active.video.metadata().decodedSequence, qint64(702));

    const OutputBusFrame& follower = result.frames.at(2);
    QCOMPARE(follower.outputFrameIndex, qint64(2));
    QCOMPARE(follower.sampledPlayheadMs, qint64(500));
    QCOMPARE(videoPts(follower), qint64(500));
    QCOMPARE(follower.video.metadata().gpuGeneration, uint64_t(8));
    QCOMPARE(follower.video.metadata().decodedSequence, qint64(801));
}

void TestOutputRuntime::multiplePlayEpochResetsCoalesceBeforeNextHoldLastLease() {
    const DeferredResetScenarioResult result = runDeferredResetScenario(true);

    QVERIFY2(result.activeLeaseEntered, "active hold-last lease did not reach submit barrier");
    QVERIFY2(result.resetsReturnedBeforeRelease,
             "three resets did not return while the hold-last lease remained active");
    QVERIFY2(result.followerStartedBeforeRelease,
             "follower dispatch did not start before the hold-last lease was released");
    QVERIFY2(result.sinkObservedResetReturnBeforeRelease,
             "hold-last sink did not observe reset completion before release");
    QVERIFY2(!result.diagnosticTimeout, "hold-last lease hit the diagnostic timeout");
    QCOMPARE(result.appliedResetsBeforeRelease, 0);
    QCOMPARE(result.appliedResetsAtFollowerSnapshot, 1);
    QCOMPARE(result.finalAppliedResets, 1);
    QCOMPARE(result.frames.size(), 3);

    const OutputBusFrame& held = result.frames.at(1);
    QCOMPARE(held.outputFrameIndex, qint64(1));
    QCOMPARE(held.sampledPlayheadMs, qint64(140));
    QCOMPARE(videoPts(held), qint64(100));
    QVERIFY(!held.video.metadata().key.isPlaceholder);
    QCOMPARE(held.video.metadata().gpuGeneration, uint64_t(7));
    QCOMPARE(held.video.metadata().decodedSequence, qint64(701));

    const OutputBusFrame& follower = result.frames.at(2);
    QCOMPARE(follower.outputFrameIndex, qint64(2));
    QCOMPARE(follower.sampledPlayheadMs, qint64(500));
    QCOMPARE(videoPts(follower), qint64(500));
    QCOMPARE(follower.video.metadata().gpuGeneration, uint64_t(8));
    QCOMPARE(follower.video.metadata().decodedSequence, qint64(801));
}

void TestOutputRuntime::immediateDispatchPreemptsCatchUpBurstAfterCurrentTick() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 40));
    cache.insertVideoFrame(video(0, 200, 90));

    std::atomic<qint64> playheadMs{100};

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    RuntimeResetDuringSubmitSink sink(2);
    OutputRuntime runtime(FrameRate::fromFraction(25, 1), 1, 4, 4);
    runtime.setSnapshotProvider([&]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state.playheadMs = playheadMs.load(std::memory_order_acquire);
        snapshot.state.playing = true;
        snapshot.state.forcePlayEpochReset = true;
        snapshot.state.selectedFeedIndex = 0;
        return snapshot;
    });
    runtime.setEndpoints({{assignment, &sink}});
    runtime.setIdentitySkip(false);

    runtime.dispatchDueTicksForTest(0);

    std::thread catchUpThread([&]() { runtime.dispatchDueTicksForTest(1000); });
    const bool catchUpEntered = sink.waitUntilEntered();
    playheadMs.store(200, std::memory_order_release);

    std::thread immediateThread([&]() { runtime.dispatchImmediate(); });
    const bool immediateQueued = runtime.waitForImmediateDispatchRequestsForTest(1, 2000);
    sink.release();

    catchUpThread.join();
    immediateThread.join();

    const QVector<OutputBusFrame> frames = sink.frames();
    QVERIFY2(catchUpEntered, "scheduled catch-up did not reach its active-submit barrier");
    QVERIFY2(immediateQueued, "immediate dispatch did not register before catch-up release");
    QVERIFY2(!sink.diagnosticTimeout(), "catch-up submit timed out waiting for release");
    QCOMPARE(frames.size(), 3);
    QCOMPARE(frames.at(1).sampledPlayheadMs, qint64(100));
    QCOMPARE(videoPts(frames.at(1)), qint64(100));
    QCOMPARE(frames.at(2).outputFrameIndex, qint64(2));
    QCOMPARE(frames.last().sampledPlayheadMs, qint64(200));
    QCOMPARE(videoPts(frames.last()), qint64(200));
}

void TestOutputRuntime::pgmCriticalImmediateDispatchSubmitsPreviewAndReportsPgmIdentity() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 1000, 88));

    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = false;
    state.selectedFeedIndex = 0;

    ThreadSafeCollectingSink previewSink(OutputTargetKind::QtPreview);
    ThreadSafeCollectingSink pgmSink(OutputTargetKind::Ndi);
    OutputRuntime runtime(FrameRate::fromFraction(60, 1), 1, 4, 4);
    runtime.setSnapshotProvider([cache, state]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = cache;
        snapshot.state = state;
        return snapshot;
    });

    OutputTargetAssignment preview;
    preview.id = QStringLiteral("pgm-preview");
    preview.sourceBus = OutputBusId::pgm();
    preview.kind = OutputTargetKind::QtPreview;
    preview.enabled = true;

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    runtime.setEndpoints({{preview, &previewSink}, {pgm, &pgmSink}});

    OutputDispatchRequest request;
    request.lane = OutputDispatchLane::PgmCritical;
    request.requiredBus = OutputBusId::pgm();
    request.requiredKind = OutputTargetKind::Ndi;
    request.requiredPlayheadMs = 1000;
    request.requireNonPlaceholder = true;

    const OutputDispatchReport report = runtime.dispatchImmediateWithReport(request);

    QVERIFY(report.requiredSubmitted);
    QCOMPARE(report.requiredIdentity.bus, OutputBusId::pgm());
    QCOMPARE(report.requiredIdentity.sampledPlayheadMs, qint64(1000));
    QCOMPARE(report.requiredIdentity.sourcePtsMs, qint64(1000));
    QVERIFY(!report.requiredIdentity.videoPlaceholder);
    QCOMPARE(pgmSink.frameCount(), 1);
    QCOMPARE(previewSink.frameCount(), 1);
    QCOMPARE(report.submittedFrames.size(), 2);
    QCOMPARE(report.submittedFrames.at(0).assignment.id, QStringLiteral("pgm-ndi"));
    QCOMPARE(report.submittedFrames.at(1).assignment.id, QStringLiteral("pgm-preview"));
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
    state.playing = true;
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
