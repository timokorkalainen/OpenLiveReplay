#include <QtTest>

#include "playback/gpu/gpupipelineconfig.h"
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <QElapsedTimer>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

class TestPlaybackWorker : public QObject {
    Q_OBJECT

private slots:
    void cleanup();
    void blockedSinkDoesNotExtendCommitLatency_data();
    void blockedSinkDoesNotExtendCommitLatency();
};

namespace {

constexpr qint64 kCommitBudgetNs = 20'000'000;
constexpr qint64 kOutlierCeilingNs = 100'000'000;
constexpr int kCommitSamples = 11;
constexpr size_t kCommitP90Index = 9; // nearest-rank p90; tolerates one scheduler outlier
static_assert(kCommitP90Index < size_t(kCommitSamples));

FrameHandle testVideoFrame(int feed, qint64 ptsMs, uchar y) {
    FrameHandle frame = solidYuv420pHandle(4, 4, y, 128, 128);
    frame.metadata().key.feedIndex = feed;
    frame.metadata().key.ptsMs = ptsMs;
    return frame;
}

class BlockingPgmSink final : public IOutputSink {
public:
    explicit BlockingPgmSink(int blockOnSubmit) : m_blockOnSubmit(blockOnSubmit) {}

    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_active = false;
            m_released = true;
        }
        m_release.notify_all();
    }

    bool isActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame&) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_active) return false;
        ++m_submitCount;
        if (m_submitCount != m_blockOnSubmit) return true;

        m_blocked = true;
        m_entered.notify_all();
        m_release.wait(lock, [this] { return m_released; });
        m_blocked = false;
        return m_active;
    }

    bool waitUntilBlocked(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_entered.wait_for(lock, timeout, [this] { return m_blocked; });
    }

    bool isBlocked() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_blocked;
    }

    int submitCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_submitCount;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_release.notify_all();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_entered;
    std::condition_variable m_release;
    bool m_active = false;
    bool m_blocked = false;
    bool m_released = false;
    int m_submitCount = 0;
    const int m_blockOnSubmit;
};

} // namespace

void TestPlaybackWorker::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
}

void TestPlaybackWorker::blockedSinkDoesNotExtendCommitLatency_data() {
    QTest::addColumn<bool>("gpuRuntimeEnabled");
    QTest::newRow("cpu-runtime") << false;
#ifdef OLR_GPU_PIPELINE_BUILD
    QTest::newRow("gpu-runtime") << true;
#endif
}

void TestPlaybackWorker::blockedSinkDoesNotExtendCommitLatency() {
    QFETCH(bool, gpuRuntimeEnabled);
    qputenv("OLR_GPU_PIPELINE", gpuRuntimeEnabled ? "1" : "0");
    QCOMPARE(gpuPipelineEnabled(), gpuRuntimeEnabled);

    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1200);
    transport.setPlaying(false);

    // The first dispatch prewarms optional GPU compositor/RHI initialization;
    // the measured active lease blocks on the second submission in both modes.
    BlockingPgmSink sink(2);
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(800, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(800, std::memory_order_release);
    // The decoded target is ready to commit. As in the production commit callers,
    // clear the pending-target marker before CommitGate validates the generation.
    worker.m_seekTargetMs = -1;
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 800, 64));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1200, 96));
        worker.publishOutputCacheLocked();
    }

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker] { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &sink}});
        runtime = worker.m_outputRuntime.get();
    }

    runtime->dispatchImmediate();
    QCOMPARE(sink.submitCount(), 1);

    auto activeDispatch =
        std::async(std::launch::async, [runtime] { return runtime->dispatchImmediate(); });
    if (!sink.waitUntilBlocked(std::chrono::seconds(2))) {
        sink.release();
        activeDispatch.wait();
        QFAIL("active sink submission did not reach its deterministic barrier");
    }

    const OutputRuntime::PlayEpochStateForTest epochStateBeforeCommits =
        runtime->playEpochStateForTest();

    struct CommitMeasurement {
        bool allCommitted = true;
        std::vector<qint64> latencyNs;
        qint64 committedPlayheadMs = -1;
        uint64_t committedGeneration = 0;
    };
    auto commit = std::async(std::launch::async, [&worker] {
        QMutexLocker workerLocker(&worker.m_mutex);
        QMutexLocker bufferLocker(&worker.m_bufferMutex);

        CommitMeasurement measurement;
        measurement.latencyNs.reserve(kCommitSamples);
        for (int sample = 0; sample < kCommitSamples; ++sample) {
            const uint64_t generation = uint64_t(8 + sample);
            worker.m_seekGeneration.store(generation, std::memory_order_release);

            PlaybackWorker::OutputCommit request;
            request.playheadMs = 1200;
            request.seekGeneration = generation;
            request.cacheAction = PlaybackWorker::OutputCacheAction::Publish;
            request.coverageMode = PlaybackWorker::OutputCoverageMode::OperatorSeek;
            request.clearSeekTarget = true;
            request.guardPlayheadCache = true;
            request.dispatch = PlaybackWorker::PostCommitDispatch::PgmCritical;

            QElapsedTimer timer;
            timer.start();
            const PlaybackWorker::OutputCommitResult result =
                worker.commitOutputStateLocked(request);
            measurement.latencyNs.push_back(timer.nsecsElapsed());
            measurement.allCommitted = measurement.allCommitted && result.committed;
            measurement.committedPlayheadMs = result.committedPlayheadMs;
            measurement.committedGeneration = result.committedGeneration;
        }
        return measurement;
    });

    const bool commitReturnedWhileBlocked =
        commit.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    if (!commitReturnedWhileBlocked) {
        sink.release();
        activeDispatch.wait();
        commit.wait();
        QFAIL("cache commit waited for the blocked output submission");
    }
    const CommitMeasurement commitMeasurement = commit.get();
    const OutputRuntime::PlayEpochStateForTest blockedEpochState = runtime->playEpochStateForTest();
    const bool activeLeaseStillBlocked =
        sink.isBlocked() &&
        activeDispatch.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;

    auto operatorPgm = std::async(std::launch::async, [&worker] {
        QElapsedTimer timer;
        timer.start();
        OutputDispatchReport report = worker.dispatchPgmAfterSeekCommit(1200);
        return std::make_pair(std::move(report), timer.nsecsElapsed());
    });
    const bool operatorQueuedBehindLease =
        runtime->waitForImmediateDispatchRequestsForTest(2, 2000);
    const bool operatorStillWaiting =
        operatorPgm.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;

    const bool workerMutexAvailable = worker.m_mutex.tryLock(20);
    bool bufferMutexAvailable = false;
    if (workerMutexAvailable) {
        bufferMutexAvailable = worker.m_bufferMutex.tryLock(20);
        if (bufferMutexAvailable) worker.m_bufferMutex.unlock();
        worker.m_mutex.unlock();
    }

    sink.release();
    activeDispatch.get();
    const OutputRuntime::PlayEpochStateForTest releasedEpochState =
        runtime->playEpochStateForTest();
    const auto [pgmReport, operatorPgmNs] = operatorPgm.get();

    std::vector<qint64> sortedLatencyNs = commitMeasurement.latencyNs;
    std::sort(sortedLatencyNs.begin(), sortedLatencyNs.end());
    const qint64 medianNs = sortedLatencyNs.at(sortedLatencyNs.size() / 2);
    const qint64 p90Ns = sortedLatencyNs.at(kCommitP90Index);
    const qint64 maximumNs = sortedLatencyNs.back();
    std::fprintf(stderr,
                 "transport_runtime_gpu=%d commit_samples=%d commit_median_ns=%lld "
                 "commit_p90_ns=%lld commit_max_ns=%lld operator_pgm_completion_ns=%lld\n",
                 gpuRuntimeEnabled ? 1 : 0, kCommitSamples, static_cast<long long>(medianNs),
                 static_cast<long long>(p90Ns), static_cast<long long>(maximumNs),
                 static_cast<long long>(operatorPgmNs));

    QVERIFY(commitMeasurement.allCommitted);
    QCOMPARE(commitMeasurement.committedPlayheadMs, qint64(1200));
    QCOMPARE(commitMeasurement.committedGeneration, uint64_t(8 + kCommitSamples - 1));
    QVERIFY2(activeLeaseStillBlocked,
             "the active sink submission must remain blocked when commit/reset returns");
    QCOMPARE(blockedEpochState.configGeneration,
             epochStateBeforeCommits.configGeneration + quint64(kCommitSamples));
    QVERIFY(blockedEpochState.pendingReset);
    QCOMPARE(blockedEpochState.appliedResetCount, epochStateBeforeCommits.appliedResetCount);
    QCOMPARE(releasedEpochState.configGeneration, blockedEpochState.configGeneration);
    QVERIFY(!releasedEpochState.pendingReset);
    QCOMPARE(releasedEpochState.appliedResetCount, epochStateBeforeCommits.appliedResetCount + 1);
    QVERIFY2(p90Ns < kCommitBudgetNs,
             qPrintable(QStringLiteral("commit/reset p90 took %1 ns; budget is %2 ns")
                            .arg(p90Ns)
                            .arg(kCommitBudgetNs)));
    QVERIFY2(maximumNs < kOutlierCeilingNs,
             qPrintable(QStringLiteral("commit/reset outlier took %1 ns; ceiling is %2 ns")
                            .arg(maximumNs)
                            .arg(kOutlierCeilingNs)));
    QVERIFY2(operatorQueuedBehindLease,
             "operator PGM completion did not register behind the active dispatch lease");
    QVERIFY2(operatorStillWaiting,
             "operator PGM completion unexpectedly bypassed the blocked sink lease");
    QVERIFY2(workerMutexAvailable && bufferMutexAvailable,
             "post-commit PGM completion retained a worker/cache lock while waiting");
    QVERIFY(pgmReport.requiredSubmitted);
    QCOMPARE(pgmReport.requiredIdentity.bus, OutputBusId::pgm());
    QCOMPARE(pgmReport.requiredIdentity.sampledPlayheadMs, qint64(1200));
    QCOMPARE(pgmReport.requiredIdentity.sourcePtsMs, qint64(1200));
    QVERIFY(!pgmReport.requiredIdentity.videoPlaceholder);
}

QTEST_APPLESS_MAIN(TestPlaybackWorker)
#include "tst_transportcommit_perf.moc"
