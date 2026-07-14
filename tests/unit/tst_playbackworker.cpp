#include <QtTest>

#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/qtpreviewsink.h"
#include "playback/output/queuedoutputsink.h"
#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpurhicontext.h"
#endif
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class TestPlaybackWorker : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void defaultPlaybackSkipsHiddenFeedPreviewEndpoints();
    void gpuFlagWrapsSinksInReadbackRing();
    void pgmPreviewGetsDepthOneRing();
    void buildsSinkForNewIoTargetKinds_data();
    void buildsSinkForNewIoTargetKinds();
    void newIoTargetsShareGpuReadbackCache();
    void selectedFeedCoverageAdvancesPlayheadWhenOtherFeedMissing();
    void pausedStepUsesPriorFrameCoverage();
    void outputCommitRejectsStaleGenerationWithoutMutation();
    void outputCommitPublishesTypedStateAndReturnsPgmDispatch();
    void outputCommitUsesDisplayableFallbackAndReturnsPreviewDispatch();
    void fullRepositionRejectedCommitRestoresLiveAndStagingCaches();
    void armedCutPromotionCommitsSubmittedIdentityAndEpoch();
    void armedCutRejectedPromotionRollsBackWithoutEpochReset();
    void coveredSeekCommitsPlayheadBeforeWorkerRuns();
    void coveredSeekPublishesLiveCacheForInstantSnapshot();
    void outputRuntimeStatsDoesNotBlockOnActiveDispatch();
    void pausedCoveredSeekDiscardsPendingOutputWithoutRewindingFrameIndex();
    void operatorSeekTransactionCompletesCoveredSeekWithPgmEvidence();
    void earlyOperatorCommitCannotExposeStaleEpoch();
    void operatorSeekTransactionPublishesTargetBeforeLeadWindowFill();
    void operatorSeekTransactionKeepsWaitingAfterEarlyPgmMiss();
    void decodedPacketsDoNotRepeatMissingPgmObligation();
    void operatorSeekTransactionTimesOutWhenSeekGenerationUncommitted();
    void operatorSeekTransactionAbandonedOnTimeout();
    void operatorSeekCompletionEmitsSignal();
    void seekToWithPgmNotifyCompletesCacheHitInline();
    void seekToWithPgmNotifyLeavesCacheMissWaiting();
    void generationBumpEmitsSupersededForWaitingTransaction();
    void abandonSuppressesLaterCompletion();
    void reuseAtAcceptsSelectedFeedWhenOtherFeedMissing();
    void reuseAtRejectsStalePriorFrameForOperatorSeek();
    void priorFrameAtFrameDurationBoundaryDoesNotCoverSeek();
    void stalePriorFrameDoesNotCoverFarSeek();
    void unbracketedPriorFrameDoesNotCoverNextSourceFrame();
    void sameCadenceFutureBeyondRoundingWindowDoesNotCoverMissingFrame();
    void sameCadenceGapNearFutureDoesNotCoverMissingFrame();
    void staleNearFutureSeekDoesNotCommitFromCache();
    void operatorSeekRejectsStaleBracketedCacheCoverage();
    void operatorTransactionDisablesDisplayableRepositionFallback();
    void liveStartupSeekCommitsFirstDisplayableFrameWhenZeroIsUncovered();
    void lowerCadenceDisplayableHoldBoundaries();
    void strictSeekNeedsPriorFrameWhenFutureIsOneTickAhead();
    void staleExactGpuFrameCannotBeCoveredByFreshNeighbors();
    void uncoveredEarlySeekFindsFirstDisplayableCachePlayhead();
    void uncoveredTailSeekFindsLastDisplayableCachePlayhead();
    void outputSnapshotIgnoresUncoveredBookmarkAndSnapsToDisplayableCache();
    void outputSnapshotAdvancesPastCoveredBookmarkToLiveTailFrame();
    void pausedWorkerKeepsWorkingUntilOutputCacheCoversPlayhead();
    void multiviewPausedCoverageAllowsBracketedLowerCadenceFrames();
    void liveDisplayableCoverageAllowsLowerCadenceHoldNearFuture();
    void multiviewCoverageHoldsPlayheadWhenAnyFeedMissing();
    void seekDirectionHintSurvivesUpdatedTransport();
    void liveGrowthProbeUsesFilesystemWhenAvioSizeIsStale();
    void liveEofRecoveryAnchorsNearNewestTail();
    void liveReadDeadlineInterruptsOnlyAfterDeadline();
    void primaryVideoPacketIndexingDoesNotRequireCodecContext();
#ifdef OLR_GPU_PIPELINE_BUILD
    void gpuBudgetConfiguredFromOutputGraphGeometry();
    void gpuBudgetUsesCodecGeometryForDecodeSurfaces();
    void gpuForceBudgetConstrainsConfiguredBudget();
    void residencyWindowParamsPreserveDefaultCap();
    void residencyWindowParamsCanExpandTrailCap();
    void outputStatsSurfaceGpuBudgetCounters();
    void gpuSeekPrefetchPlanUsesBudgetHeadroom();
    void gpuSeekPrefetchPlanUsesCodecGeometryForSurfaceBytes();
    void gpuSeekPrefetchAllowanceBoundsNativeGpuDecode();
    void gpuSeekPrefetchAllowanceUsesPlannedWindow();
    void gpuSeekPrefetchDoesNotExtendManualSeekCommitFill();
    void gpuPressureDerivesRuntimeBudgetFromHeadroom();
    void gpuPressureWarningShrinksWindowWithoutLatch();
    void gpuPressureBelowLevel1LatchesCpuBeforeJetsam();
    void gpuPressureLevel2LatchesCpuWithoutDeviceLoss();
    void gpuPressureRecoveryReanchorsCommittedPlayheadDuringPendingSeek();
    void gpuPressureRecoveryHonorsSelectedFeedMode();
    void countersExposeGpuFallbackOutputGateState();
    void initializeOutputGraphClearsMemoryPressureLatch();
    void initializeOutputGraphCommitsPlaceholderGenerationAndEpoch();

private:
    bool installTestGpuSpine(PlaybackWorker& worker) const;
#endif
};

namespace {

OutputTargetAssignment ndiFeedAssignment() {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::Ndi;
    assignment.enabled = true;
    return assignment;
}

OutputTargetAssignment feedAssignment(OutputTargetKind kind) {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-%1").arg(outputTargetKindName(kind));
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = kind;
    assignment.enabled = true;
    return assignment;
}

const OutputEndpoint* findEndpoint(const QList<OutputEndpoint>& endpoints, OutputTargetKind kind,
                                   OutputBusId bus) {
    for (const OutputEndpoint& endpoint : endpoints) {
        if (endpoint.assignment.kind == kind && endpoint.assignment.sourceBus == bus)
            return &endpoint;
    }
    return nullptr;
}

const AsyncGpuReadbackSink* asAsyncSink(const OutputEndpoint* endpoint) {
    return endpoint ? dynamic_cast<const AsyncGpuReadbackSink*>(endpoint->sink) : nullptr;
}

class CountingDiscardSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame&) override { return m_active; }
    void discardPending() override { ++discardPendingCalls; }

    int discardPendingCalls = 0;

private:
    bool m_active = false;
};

class BlockingSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }

    void stop() override { release(); }
    bool isActive() const override { return m_active; }

    bool submit(const OutputBusFrame&) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_insideSubmit = true;
        m_entered.notify_all();
        m_release.wait(lock, [this] { return m_released; });
        return m_active;
    }

    bool waitUntilInsideSubmit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_entered.wait_for(lock, timeout, [this] { return m_insideSubmit; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_release.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_entered;
    std::condition_variable m_release;
    bool m_active = false;
    bool m_insideSubmit = false;
    bool m_released = false;
};

class CommitBarrier final : public PlaybackWorker::OutputCommitBarrierForTest {
public:
    void enterAndWait() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_inside = true;
        m_entered.notify_all();
        m_release.wait(lock, [this] { return m_released; });
    }

    bool waitUntilEntered(int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_entered.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [this] { return m_inside; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_release.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_entered;
    std::condition_variable m_release;
    bool m_inside = false;
    bool m_released = false;
};

class CommitBarrierReleaseGuard final {
public:
    explicit CommitBarrierReleaseGuard(CommitBarrier& barrier) : m_barrier(barrier) {}
    ~CommitBarrierReleaseGuard() { m_barrier.release(); }

private:
    CommitBarrier& m_barrier;
};

class OutputCommitBarrierInstallGuard final {
public:
    OutputCommitBarrierInstallGuard(PlaybackWorker& worker, CommitBarrier& barrier)
        : m_worker(worker) {
        m_worker.setOutputCommitBarrierForTest(&barrier);
    }
    ~OutputCommitBarrierInstallGuard() { m_worker.setOutputCommitBarrierForTest(nullptr); }

private:
    PlaybackWorker& m_worker;
};

class TestPgmSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override {
        if (!m_active) return false;
        ++submitAttempts;
        if (rejectSubmits) return false;
        frames.append(frame);
        return true;
    }
    QVector<OutputBusFrame> frames;
    int submitAttempts = 0;
    bool rejectSubmits = false;

private:
    bool m_active = false;
};

FrameHandle testVideoFrame(int feed, qint64 ptsMs, uchar y) {
    FrameHandle frame = solidYuv420pHandle(4, 4, y, 128, 128);
    frame.metadata().key.feedIndex = feed;
    frame.metadata().key.ptsMs = ptsMs;
    return frame;
}

} // namespace

void TestPlaybackWorker::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
    qunsetenv("OLR_GPU_FORCE_BUDGET");
    qunsetenv("OLR_LEDGER_REPORT_ONLY");
#ifdef OLR_GPU_PIPELINE_BUILD
    GpuBudget::instance().reset();
#endif
}

void TestPlaybackWorker::defaultPlaybackSkipsHiddenFeedPreviewEndpoints() {
    FrameProvider feedProvider;
    FrameProvider multiviewProvider;
    FrameProvider pgmProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setBusPreviewProviders(&multiviewProvider, &pgmProvider);
    worker.initializeOutputGraph(1, 4, 4);

    QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
    const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();

    QVERIFY(!findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::feed(0)));
    QVERIFY(findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::multiview()));
    QVERIFY(findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::pgm()));
}

void TestPlaybackWorker::liveGrowthProbeUsesFilesystemWhenAvioSizeIsStale() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("growing.mkv"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("grow", 4), qint64(4));
    file.close();

    QCOMPARE(PlaybackWorker::liveGrowthFileSizeForTest(/*avioSize=*/1, path), int64_t(4));
    QCOMPARE(PlaybackWorker::liveGrowthFileSizeForTest(/*avioSize=*/9, path), int64_t(9));
    QCOMPARE(PlaybackWorker::liveGrowthFileSizeForTest(/*avioSize=*/123,
                                                       dir.filePath(QStringLiteral("missing.mkv"))),
             int64_t(123));
    QCOMPARE(PlaybackWorker::liveGrowthFileSizeForTest(/*avioSize=*/-1,
                                                       dir.filePath(QStringLiteral("missing.mkv"))),
             int64_t(-1));
}

void TestPlaybackWorker::liveEofRecoveryAnchorsNearNewestTail() {
    const qint64 playheadMs = 2569;
    const qint64 newestBeforeEofMs = 2620;
    const qint64 trailMs = 300;
    const qint64 frameDurationMs = 40;

    const qint64 anchor = PlaybackWorker::liveEofRecoveryAnchorMsForTest(
        playheadMs, newestBeforeEofMs, trailMs, frameDurationMs);

    QVERIFY2(
        anchor > playheadMs - trailMs,
        qPrintable(QStringLiteral("anchor=%1 oldAnchor=%2").arg(anchor).arg(playheadMs - trailMs)));
    QCOMPARE(anchor, newestBeforeEofMs - frameDurationMs);
}

void TestPlaybackWorker::liveReadDeadlineInterruptsOnlyAfterDeadline() {
    QVERIFY(!PlaybackWorker::liveReadDeadlineInterruptsForTest(/*baseInterrupt=*/false,
                                                               /*deadlineMs=*/-1,
                                                               /*nowMs=*/100));
    QVERIFY(PlaybackWorker::liveReadDeadlineInterruptsForTest(/*baseInterrupt=*/true,
                                                              /*deadlineMs=*/-1,
                                                              /*nowMs=*/100));
    QVERIFY(!PlaybackWorker::liveReadDeadlineInterruptsForTest(/*baseInterrupt=*/false,
                                                               /*deadlineMs=*/200,
                                                               /*nowMs=*/199));
    QVERIFY(PlaybackWorker::liveReadDeadlineInterruptsForTest(/*baseInterrupt=*/false,
                                                              /*deadlineMs=*/200,
                                                              /*nowMs=*/200));
}

#ifdef OLR_GPU_PIPELINE_BUILD
bool TestPlaybackWorker::installTestGpuSpine(PlaybackWorker& worker) const {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) rhi = GpuRhiContext::createWarpForTest();
    if (!rhi || !rhi->isValid()) return false;

    worker.m_gpuRhi = std::move(rhi);
    worker.m_decodeFence = DecodeDoneFence::create();
    worker.m_renderFence = worker.m_gpuRhi->createFence();
    worker.m_stagingFence = worker.m_gpuRhi->createFence();
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    return true;
}
#endif

void TestPlaybackWorker::gpuFlagWrapsSinksInReadbackRing() {
    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    {
        qunsetenv("OLR_GPU_PIPELINE");
        PlaybackWorker worker({&feedProvider}, &transport);
        worker.setFeedPreviewProvidersEnabled(true);
        worker.setExternalOutputTargets({ndiFeedAssignment()});
        worker.initializeOutputGraph(1, 4, 4);

        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();
        const OutputEndpoint* preview =
            findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::feed(0));
        const OutputEndpoint* ndi =
            findEndpoint(endpoints, OutputTargetKind::Ndi, OutputBusId::feed(0));
        QVERIFY(preview);
        QVERIFY(ndi);
        QVERIFY(!asAsyncSink(preview));
        QVERIFY(!asAsyncSink(ndi));
        QVERIFY(dynamic_cast<const QtPreviewOutputSink*>(preview->sink));
        QVERIFY(dynamic_cast<const QueuedOutputSink*>(ndi->sink));
    }

    {
        qputenv("OLR_GPU_PIPELINE", "1");
        PlaybackWorker worker({&feedProvider}, &transport);
        worker.setFeedPreviewProvidersEnabled(true);
        worker.setExternalOutputTargets({ndiFeedAssignment()});
        worker.initializeOutputGraph(1, 4, 4);
#ifdef OLR_GPU_PIPELINE_BUILD
        if (!worker.gpuPathActive()) {
            QVERIFY(installTestGpuSpine(worker));
            worker.rebuildOutputEndpoints();
        }
#endif

        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();
        const OutputEndpoint* preview =
            findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::feed(0));
        const OutputEndpoint* ndi =
            findEndpoint(endpoints, OutputTargetKind::Ndi, OutputBusId::feed(0));
        const AsyncGpuReadbackSink* previewReadback = asAsyncSink(preview);
        const AsyncGpuReadbackSink* ndiReadback = asAsyncSink(ndi);
        QVERIFY(previewReadback);
        QVERIFY(ndiReadback);
        QCOMPARE(previewReadback->ringDepth(), 1);
        QCOMPARE(ndiReadback->ringDepth(), 3);
        QVERIFY(previewReadback->sharedReadbacks());
        QCOMPARE(previewReadback->sharedReadbacks(), worker.m_outputRuntime->sharedGpuReadbacks());
        QCOMPARE(ndiReadback->sharedReadbacks(), worker.m_outputRuntime->sharedGpuReadbacks());
    }
}

void TestPlaybackWorker::pgmPreviewGetsDepthOneRing() {
    qputenv("OLR_GPU_PIPELINE", "1");
    FrameProvider feedProvider;
    FrameProvider pgmProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setFeedPreviewProvidersEnabled(true);
    worker.setBusPreviewProviders(nullptr, &pgmProvider);
    worker.initializeOutputGraph(1, 4, 4);
#ifdef OLR_GPU_PIPELINE_BUILD
    if (!worker.gpuPathActive()) {
        QVERIFY(installTestGpuSpine(worker));
        worker.rebuildOutputEndpoints();
    }
#endif

    QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
    const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();
    const AsyncGpuReadbackSink* feedPreview =
        asAsyncSink(findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::feed(0)));
    const AsyncGpuReadbackSink* pgmPreview =
        asAsyncSink(findEndpoint(endpoints, OutputTargetKind::QtPreview, OutputBusId::pgm()));

    QVERIFY(feedPreview);
    QVERIFY(pgmPreview);
    QCOMPARE(feedPreview->ringDepth(), 1);
    QCOMPARE(pgmPreview->ringDepth(), 1);
    QCOMPARE(feedPreview->sharedReadbacks(), worker.m_outputRuntime->sharedGpuReadbacks());
    QCOMPARE(pgmPreview->sharedReadbacks(), worker.m_outputRuntime->sharedGpuReadbacks());
}

void TestPlaybackWorker::buildsSinkForNewIoTargetKinds_data() {
    QTest::addColumn<int>("kind");

    QTest::newRow("decklink-sdi-hdmi") << static_cast<int>(OutputTargetKind::DeckLinkSdiHdmi);
    QTest::newRow("decklink-ip-st2110") << static_cast<int>(OutputTargetKind::DeckLinkIpSt2110);
    QTest::newRow("aja") << static_cast<int>(OutputTargetKind::Aja);
    QTest::newRow("omt") << static_cast<int>(OutputTargetKind::Omt);
}

void TestPlaybackWorker::buildsSinkForNewIoTargetKinds() {
    QFETCH(int, kind);
    const auto targetKind = static_cast<OutputTargetKind>(kind);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({feedAssignment(targetKind)});
    worker.initializeOutputGraph(1, 4, 4);

    QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
    const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();
    const OutputEndpoint* endpoint = findEndpoint(endpoints, targetKind, OutputBusId::feed(0));
    QVERIFY(endpoint);
    QVERIFY(endpoint->sink);
    QCOMPARE(endpoint->sink->kind(), targetKind);
    QVERIFY(!endpoint->sink->isActive());

    const AsyncGpuReadbackSink* readback = asAsyncSink(endpoint);
    if (targetKind == OutputTargetKind::DeckLinkSdiHdmi ||
        targetKind == OutputTargetKind::DeckLinkIpSt2110) {
        QVERIFY(readback == nullptr);
    } else {
        QVERIFY(readback);
        QCOMPARE(readback->ringDepth(), 3);
    }
}

void TestPlaybackWorker::newIoTargetsShareGpuReadbackCache() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({feedAssignment(OutputTargetKind::Aja)});
    worker.initializeOutputGraph(1, 4, 4);
#ifdef OLR_GPU_PIPELINE_BUILD
    if (!worker.gpuPathActive()) {
        QVERIFY(installTestGpuSpine(worker));
        worker.rebuildOutputEndpoints();
    }
#endif

    QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
    const QList<OutputEndpoint>& endpoints = worker.m_outputRuntime->outputEndpointsForTest();
    const OutputEndpoint* endpoint =
        findEndpoint(endpoints, OutputTargetKind::Aja, OutputBusId::feed(0));
    QVERIFY(endpoint);
    const AsyncGpuReadbackSink* readback = asAsyncSink(endpoint);
    QVERIFY(readback);
    QVERIFY(readback->sharedReadbacks());
    QCOMPARE(readback->sharedReadbacks(), worker.m_outputRuntime->sharedGpuReadbacks());
}

void TestPlaybackWorker::selectedFeedCoverageAdvancesPlayheadWhenOtherFeedMissing() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
        worker.publishOutputCacheLocked();
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.selectedFeedIndex, 0);
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    QVERIFY(!snapshot.state.forcePlayEpochReset);
}

void TestPlaybackWorker::pausedStepUsesPriorFrameCoverage() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 900, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1100, 90));
        worker.publishOutputCacheLocked();
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    QVERIFY(!snapshot.state.forcePlayEpochReset);
}

void TestPlaybackWorker::outputCommitRejectsStaleGenerationWithoutMutation() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(480, std::memory_order_release);
    worker.m_outputPlayheadCacheGuarded.store(true, std::memory_order_release);
#ifdef OLR_GPU_PIPELINE_BUILD
    worker.m_committedGpuGeneration.store(17, std::memory_order_release);
#endif
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
    }

    PlaybackWorker::OutputCommitResult result;
    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_seekTargetMs = -1;
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1200, 96));

        PlaybackWorker::OutputCommit commit;
        commit.playheadMs = 1200;
        commit.seekGeneration = 7;
        commit.gpuGeneration = 23;
        commit.cacheAction = PlaybackWorker::OutputCacheAction::Publish;
        commit.coverageMode = PlaybackWorker::OutputCoverageMode::OperatorSeek;
        commit.guardPlayheadCache = false;
        commit.dispatch = PlaybackWorker::PostCommitDispatch::PgmCritical;
        result = worker.commitOutputStateLocked(commit);
    }

    QVERIFY(!result.committed);
    QCOMPARE(result.dispatch, PlaybackWorker::PostCommitDispatch::None);
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(500));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(480));
    QVERIFY(worker.m_outputPlayheadCacheGuarded.load(std::memory_order_acquire));
#ifdef OLR_GPU_PIPELINE_BUILD
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), uint64_t(17));
#endif
    QVERIFY(!worker.m_publishedCache.load());
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 0);
    }
}

void TestPlaybackWorker::outputCommitPublishesTypedStateAndReturnsPgmDispatch() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
    }

    PlaybackWorker::OutputCommitResult result;
    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_seekTargetMs = -1;
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1200, 96));

        PlaybackWorker::OutputCommit commit;
        commit.playheadMs = 1200;
        commit.seekGeneration = 8;
        commit.gpuGeneration = 23;
        commit.cacheAction = PlaybackWorker::OutputCacheAction::Publish;
        commit.coverageMode = PlaybackWorker::OutputCoverageMode::OperatorSeek;
        commit.guardPlayheadCache = true;
        commit.dispatch = PlaybackWorker::PostCommitDispatch::PgmCritical;
        result = worker.commitOutputStateLocked(commit);
    }

    QVERIFY(result.committed);
    QCOMPARE(result.committedPlayheadMs, qint64(1200));
    QCOMPARE(result.committedGeneration, uint64_t(8));
    QCOMPARE(result.dispatch, PlaybackWorker::PostCommitDispatch::PgmCritical);
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1200));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(1200));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(8));
    QVERIFY(worker.m_outputPlayheadCacheGuarded.load(std::memory_order_acquire));
#ifdef OLR_GPU_PIPELINE_BUILD
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), uint64_t(23));
#endif
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    QVERIFY(published->videoFrameAt(0, 1200).has_value());
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 1);
    }
}

void TestPlaybackWorker::outputCommitUsesDisplayableFallbackAndReturnsPreviewDispatch() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(5, std::memory_order_release);
    worker.m_committedGeneration.store(4, std::memory_order_release);
    worker.m_outputPlayheadCacheGuarded.store(true, std::memory_order_release);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(30, 1), 1, 4, 4);
    }

    PlaybackWorker::OutputCommitResult result;
    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_seekTargetMs = 0;
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 832, 80));

        PlaybackWorker::OutputCommit commit;
        commit.playheadMs = 0;
        commit.seekGeneration = 5;
        commit.cacheAction = PlaybackWorker::OutputCacheAction::Publish;
        commit.coverageMode = PlaybackWorker::OutputCoverageMode::Displayable;
        commit.requireCurrentSeek = false;
        commit.clearSeekTarget = true;
        commit.guardPlayheadCache = false;
        commit.dispatch = PlaybackWorker::PostCommitDispatch::Preview;
        result = worker.commitOutputStateLocked(commit);
    }

    QVERIFY(result.committed);
    QCOMPARE(result.committedPlayheadMs, qint64(832));
    QCOMPARE(result.committedGeneration, uint64_t(5));
    QCOMPARE(result.dispatch, PlaybackWorker::PostCommitDispatch::Preview);
    QCOMPARE(worker.m_seekTargetMs, qint64(-1));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(832));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(832));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(5));
    QVERIFY(!worker.m_outputPlayheadCacheGuarded.load(std::memory_order_acquire));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 1);
    }
}

void TestPlaybackWorker::fullRepositionRejectedCommitRestoresLiveAndStagingCaches() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(9, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(480, std::memory_order_release);
    worker.m_outputPlayheadCacheGuarded.store(true, std::memory_order_release);
#ifdef OLR_GPU_PIPELINE_BUILD
    worker.m_committedGpuGeneration.store(17, std::memory_order_release);
#endif
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
    }

    std::unique_ptr<OutputFrameCache> liveSaved = std::make_unique<OutputFrameCache>(1, 4, 4);
    liveSaved->insertVideoFrame(testVideoFrame(0, 100, 48));
    liveSaved->insertVideoFrame(testVideoFrame(0, 500, 64));
    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1200, 96));
    worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1240, 104));
    OutputFrameCache* const stagingBefore = worker.m_outputCache.get();
    OutputFrameCache* const liveBefore = liveSaved.get();

    auto publishedBefore = std::make_shared<const OutputFrameCache>(*liveSaved);
    worker.m_publishedCache.publish(publishedBefore);

    PlaybackWorker::OutputCommitResult result;
    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_seekTargetMs = -1;
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        PlaybackWorker::OutputCommit commit;
        commit.playheadMs = 1200;
        commit.seekGeneration = 8; // stale: current generation is 9
        commit.gpuGeneration = 23;
        commit.cacheAction = PlaybackWorker::OutputCacheAction::MergeStagingAndPublish;
        commit.coverageMode = PlaybackWorker::OutputCoverageMode::OperatorSeek;
        commit.guardPlayheadCache = false;
        commit.dispatch = PlaybackWorker::PostCommitDispatch::Output;
        result = worker.commitFullRepositionOutputStateLocked(
            commit, liveSaved, /*keepFrom=*/900, /*keepTo=*/1500,
            /*keepAudioFromSample=*/43200, /*sanitizeForDeviceLoss=*/false);
    }

    QVERIFY(!result.committed);
    QCOMPARE(result.dispatch, PlaybackWorker::PostCommitDispatch::None);
    QCOMPARE(worker.m_outputCache.get(), stagingBefore);
    QCOMPARE(liveSaved.get(), liveBefore);
    QVERIFY(!worker.m_stagingCache);
    QCOMPARE(worker.m_publishedCache.load().get(), publishedBefore.get());
    QCOMPARE(worker.m_outputCache->videoFramesSnapshot().size(), qsizetype(2));
    QVERIFY(worker.m_outputCache->videoFrameAt(0, 1200).has_value());
    QVERIFY(worker.m_outputCache->videoFrameAt(0, 1240).has_value());
    QCOMPARE(liveSaved->videoFramesSnapshot().size(), qsizetype(2));
    QVERIFY(liveSaved->videoFrameAt(0, 100).has_value());
    QVERIFY(liveSaved->videoFrameAt(0, 500).has_value());
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(500));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(480));
    QVERIFY(worker.m_outputPlayheadCacheGuarded.load(std::memory_order_acquire));
#ifdef OLR_GPU_PIPELINE_BUILD
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), uint64_t(17));
#endif
    QCOMPARE(worker.m_seekTargetMs, qint64(-1));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 0);
    }
}

void TestPlaybackWorker::armedCutPromotionCommitsSubmittedIdentityAndEpoch() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);
    transport.setPlaying(true);

    TestPgmSink sink;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(1000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);
#ifdef OLR_GPU_PIPELINE_BUILD
    const uint64_t gpuGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(gpuGeneration, std::memory_order_release);
#endif
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 64));
        worker.publishOutputCacheLocked();
        worker.m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_prerollStagingCache->insertVideoFrame(testVideoFrame(0, 2000, 96));
    }
    worker.m_scheduledCutFrame.store(0, std::memory_order_release);
    worker.m_scheduledCutTargetMs.store(2000, std::memory_order_release);
    worker.m_armSeekGen.store(7, std::memory_order_release);
    worker.m_stagingCovers.store(true, std::memory_order_release);
    worker.m_cutArmed.store(true, std::memory_order_release);

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{feedAssignment(OutputTargetKind::Ndi), &sink}});
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 0);
    }

    worker.m_outputRuntime->dispatchDueTicksForTest(0);

    QCOMPARE(sink.frames.size(), 1);
    const OutputFrameIdentity identity = sink.frames.constFirst().identity;
    QCOMPARE(identity.sampledPlayheadMs, qint64(2000));
    QCOMPARE(identity.sourcePtsMs, qint64(2000));
    QVERIFY(!identity.videoPlaceholder);
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(2000));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(2000));
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(7));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
#ifdef OLR_GPU_PIPELINE_BUILD
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), gpuGeneration);
#endif
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 1);
        worker.m_outputRuntime->setEndpoints({});
    }
}

void TestPlaybackWorker::armedCutRejectedPromotionRollsBackWithoutEpochReset() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(1000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 64));
        worker.publishOutputCacheLocked();
        worker.m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_prerollStagingCache->insertVideoFrame(testVideoFrame(0, 400, 96));
    }
    OutputFrameCache* const liveBefore = worker.m_outputCache.get();
    OutputFrameCache* const stagingBefore = worker.m_prerollStagingCache.get();
    const std::shared_ptr<const OutputFrameCache> publishedBefore = worker.m_publishedCache.load();
    worker.m_scheduledCutFrame.store(0, std::memory_order_release);
    worker.m_scheduledCutTargetMs.store(2000, std::memory_order_release);
    worker.m_armSeekGen.store(7, std::memory_order_release);
    worker.m_stagingCovers.store(true, std::memory_order_release);
    worker.m_cutArmed.store(true, std::memory_order_release);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();

    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    QCOMPARE(worker.m_outputCache.get(), liveBefore);
    QCOMPARE(worker.m_prerollStagingCache.get(), stagingBefore);
    QCOMPARE(worker.m_publishedCache.load().get(), publishedBefore.get());
    QCOMPARE(transport.currentPos(), qint64(1000));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
    QVERIFY(worker.m_cutArmed.load(std::memory_order_acquire));
    QCOMPARE(worker.m_cutsFired.load(std::memory_order_acquire), qint64(0));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 0);
    }
}

void TestPlaybackWorker::coveredSeekCommitsPlayheadBeforeWorkerRuns() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_requireAllOutputFeedsForPlayhead.store(true, std::memory_order_release);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 1000, 120));
        worker.publishOutputCacheLocked();
    }

    worker.seekTo(1000, -1);

    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(8));
    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
}

void TestPlaybackWorker::coveredSeekPublishesLiveCacheForInstantSnapshot() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.publishOutputCacheLocked();
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
    }

    worker.seekTo(1000, -1);

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    const std::optional<FrameHandle> frame =
        snapshot.cache.videoFrameAtFreshForGeneration(0, 1000, snapshot.state.gpuGeneration);
    QVERIFY(frame.has_value());
    QVERIFY(!frame->metadata().key.isPlaceholder);
    QCOMPARE(frame->metadata().key.ptsMs, qint64(1000));
}

void TestPlaybackWorker::outputRuntimeStatsDoesNotBlockOnActiveDispatch() {
    OutputRuntime runtime(FrameRate::fromFraction(50, 1), 1, 4, 4);
    BlockingSink sink;

    runtime.setSnapshotProvider([] {
        OutputRuntimeSnapshot snapshot;
        snapshot.cache = OutputFrameCache(1, 4, 4);
        snapshot.cache.insertVideoFrame(testVideoFrame(0, 0, 80));
        snapshot.state.playing = true;
        snapshot.state.playheadMs = 0;
        return snapshot;
    });
    runtime.setEndpoints({{feedAssignment(OutputTargetKind::QtPreview), &sink}});

    std::thread dispatchThread([&runtime] { runtime.dispatchDueTicksForTestNs(0); });
    QVERIFY(sink.waitUntilInsideSubmit(std::chrono::milliseconds(500)));

    auto statsFuture = std::async(std::launch::async, [&runtime] { return runtime.stats(); });
    const bool returnedBeforeRelease =
        statsFuture.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready;

    sink.release();
    if (dispatchThread.joinable()) dispatchThread.join();
    const OutputDispatchStats stats = statsFuture.get();
    runtime.setEndpoints({});

    QVERIFY2(returnedBeforeRelease,
             "OutputRuntime::stats() must return cached stats without waiting for active dispatch");
    QCOMPARE(stats.ticks, qint64(0));
}

void TestPlaybackWorker::pausedCoveredSeekDiscardsPendingOutputWithoutRewindingFrameIndex() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
        worker.publishOutputCacheLocked();
    }
    CountingDiscardSink observedSink;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(50, 1), 1, 4, 4);
        worker.m_outputRuntime->setEndpoints(
            {{feedAssignment(OutputTargetKind::QtPreview), &observedSink}});
        worker.m_outputRuntime->dispatchImmediate();
        QCOMPARE(worker.m_outputRuntime->dispatcherNextOutputFrameIndex(), qint64(1));
    }

    int resetCount = -1;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCount = worker.m_outputRuntime->playEpochResetCountForTest();
        QCOMPARE(resetCount, 0);
        QCOMPARE(observedSink.discardPendingCalls, 0);
    }

    worker.seekTo(1000, -1);

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->setEndpoints({});
        resetCount = worker.m_outputRuntime->playEpochResetCountForTest();
        QCOMPARE(worker.m_outputRuntime->dispatcherNextOutputFrameIndex(), qint64(2));
    }
    QCOMPARE(resetCount, 1);
    QCOMPARE(observedSink.discardPendingCalls, 1);
}

void TestPlaybackWorker::operatorSeekTransactionCompletesCoveredSeekWithPgmEvidence() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

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

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(60, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    const PlaybackWorker::OperatorSeekResult result = worker.seekToAndWaitForPgm(1000, -1, 50);

    QVERIFY(result.completed);
    QVERIFY(result.submittedPgm);
    QVERIFY(!result.timedOut);
    QCOMPARE(result.targetMs, qint64(1000));
    QCOMPARE(result.generation, uint64_t(8));
    QCOMPARE(result.pgmIdentity.bus, OutputBusId::pgm());
    QCOMPARE(result.pgmIdentity.sampledPlayheadMs, qint64(1000));
    QCOMPARE(result.pgmIdentity.sourcePtsMs, qint64(1000));
    QVERIFY(!result.pgmIdentity.videoPlaceholder);
    QCOMPARE(pgmSink.frames.size(), 1);
}

void TestPlaybackWorker::earlyOperatorCommitCannotExposeStaleEpoch() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(800);
    transport.setPlaying(true);

    TestPgmSink pgmSink;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(800, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(800, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 800, 64));
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
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
        runtime = worker.m_outputRuntime.get();
    }

    runtime->dispatchDueTicksForTest(0);
    QCOMPARE(pgmSink.frames.size(), 1);
    QCOMPARE(pgmSink.frames.last().identity.sampledPlayheadMs, qint64(800));
    QCOMPARE(pgmSink.frames.last().identity.sourcePtsMs, qint64(800));

    transport.seek(1200);
    CommitBarrier barrier;
    OutputCommitBarrierInstallGuard barrierInstall(worker, barrier);
    auto seek =
        std::async(std::launch::async, [&] { return worker.seekToAndWaitForPgm(1200, 1, 2000); });

    bool transactionRegistered = false;
    const auto registrationDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < registrationDeadline) {
        {
            QMutexLocker locker(&worker.m_mutex);
            transactionRegistered = worker.m_operatorSeekCompletion.waiting &&
                                    worker.m_operatorSeekCompletion.generation == 8 &&
                                    worker.m_operatorSeekCompletion.targetMs == 1200;
        }
        if (transactionRegistered) break;
        std::this_thread::yield();
    }
    QVERIFY2(transactionRegistered, "operator seek transaction was not registered");

    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_seekTargetMs = -1;
    }

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1200, 96));
    }

    auto complete = std::async(std::launch::async, [&] {
        return worker.tryCompleteOperatorSeekFromCurrentOutputCache(1200, 8);
    });
    CommitBarrierReleaseGuard releaseGuard(barrier);
    QVERIFY(barrier.waitUntilEntered(2000));

    const int framesBeforeGapTick = pgmSink.frames.size();
    runtime->dispatchDueTicksForTest(1200);
    QVector<OutputFrameIdentity> gapIdentities;
    for (int i = framesBeforeGapTick; i < pgmSink.frames.size(); ++i)
        gapIdentities.append(pgmSink.frames.at(i).identity);

    barrier.release();
    const bool completedFromCache = complete.get();
    const auto result = seek.get();

    QVERIFY(completedFromCache);
    QVERIFY(result.submittedPgm);
    QCOMPARE(pgmSink.frames.last().identity.sampledPlayheadMs, qint64(1200));
    QCOMPARE(pgmSink.frames.last().identity.sourcePtsMs, qint64(1200));
    QVERIFY(!pgmSink.frames.last().identity.videoPlaceholder);
    QVERIFY2(!gapIdentities.isEmpty(), "background output tick did not submit a frame");
    for (const OutputFrameIdentity& identity : gapIdentities) {
        const QString diagnostic = QStringLiteral("gap frame sampled=%1 source=%2 placeholder=%3")
                                       .arg(identity.sampledPlayheadMs)
                                       .arg(identity.sourcePtsMs)
                                       .arg(identity.videoPlaceholder ? 1 : 0);
        QVERIFY2(identity.sampledPlayheadMs >= 1200 && identity.sourcePtsMs == 1200 &&
                     !identity.videoPlaceholder,
                 qPrintable(diagnostic));
    }
}

void TestPlaybackWorker::operatorSeekTransactionPublishesTargetBeforeLeadWindowFill() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);
    worker.m_seekTargetMs = -1;
    worker.m_operatorSeekCompletion = PlaybackWorker::OperatorSeekCompletionState{};
    worker.m_operatorSeekCompletion.generation = 8;
    worker.m_operatorSeekCompletion.targetMs = 1000;
    worker.m_operatorSeekCompletion.waiting = true;

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 88));
    }

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    QVERIFY(worker.tryCompleteOperatorSeekFromCurrentOutputCache(1000, 8));

    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QVERIFY(worker.m_operatorSeekCompletion.completed);
    QVERIFY(worker.m_operatorSeekCompletion.submittedPgm);
    QCOMPARE(worker.m_operatorSeekCompletion.message, QStringLiteral("PGM submitted"));
    QCOMPARE(pgmSink.frames.size(), 1);
    QCOMPARE(pgmSink.frames.first().identity.bus, OutputBusId::pgm());
    QCOMPARE(pgmSink.frames.first().identity.sampledPlayheadMs, qint64(1000));
    QCOMPARE(pgmSink.frames.first().identity.sourcePtsMs, qint64(1000));
    QVERIFY(!pgmSink.frames.first().identity.videoPlaceholder);
}

void TestPlaybackWorker::operatorSeekTransactionKeepsWaitingAfterEarlyPgmMiss() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
    pgmSink.rejectSubmits = true;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);
    worker.m_seekTargetMs = -1;
    worker.m_operatorSeekCompletion = PlaybackWorker::OperatorSeekCompletionState{};
    worker.m_operatorSeekCompletion.generation = 8;
    worker.m_operatorSeekCompletion.targetMs = 1000;
    worker.m_operatorSeekCompletion.waiting = true;

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 88));
    }

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-ndi");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    bool operatorPgmCompletedEarly = false;
    for (int packet = 0; packet < 5; ++packet) {
        worker.maybeCompleteOperatorSeekAfterDecodedPacket(1000, 8, operatorPgmCompletedEarly);
    }

    QVERIFY(!operatorPgmCompletedEarly);
    QCOMPARE(pgmSink.submitAttempts, 2);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 1);
    }
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QVERIFY(worker.m_operatorSeekCompletion.waiting);
    QVERIFY(worker.m_operatorSeekCompletion.pgmDispatchAttempted);
    QVERIFY(!worker.m_operatorSeekCompletion.completed);
    QVERIFY(!worker.m_operatorSeekCompletion.submittedPgm);

    pgmSink.rejectSubmits = false;
    const PlaybackWorker::OperatorSeekResult nextSeek = worker.seekToAndWaitForPgm(1000, 1, 50);

    const QString nextSeekDiagnostic =
        QStringLiteral("message=%1 generation=%2 committedGeneration=%3 attempts=%4")
            .arg(nextSeek.message)
            .arg(nextSeek.generation)
            .arg(worker.m_committedGeneration.load(std::memory_order_acquire))
            .arg(pgmSink.submitAttempts);
    QVERIFY2(nextSeek.completed, qPrintable(nextSeekDiagnostic));
    QVERIFY(nextSeek.submittedPgm);
    QCOMPARE(nextSeek.generation, uint64_t(9));
    QCOMPARE(nextSeek.targetMs, qint64(1000));
    QCOMPARE(pgmSink.submitAttempts, 3);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 2);
    }
}

void TestPlaybackWorker::decodedPacketsDoNotRepeatMissingPgmObligation() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);
    worker.m_seekTargetMs = -1;
    worker.m_operatorSeekCompletion = PlaybackWorker::OperatorSeekCompletionState{};
    worker.m_operatorSeekCompletion.generation = 8;
    worker.m_operatorSeekCompletion.targetMs = 1000;
    worker.m_operatorSeekCompletion.waiting = true;

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 88));
    }

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({});
    }

    bool operatorPgmCompletedEarly = false;
    for (int packet = 0; packet < 5; ++packet) {
        worker.maybeCompleteOperatorSeekAfterDecodedPacket(1000, 8, operatorPgmCompletedEarly);
    }

    QVERIFY(!operatorPgmCompletedEarly);
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QVERIFY(worker.m_operatorSeekCompletion.waiting);
    QVERIFY(worker.m_operatorSeekCompletion.pgmDispatchAttempted);
    QVERIFY(!worker.m_operatorSeekCompletion.completed);
    QVERIFY(!worker.m_operatorSeekCompletion.submittedPgm);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 1);
        QCOMPARE(worker.m_outputRuntime->dispatcherNextOutputFrameIndex(), qint64(2));
    }
}

void TestPlaybackWorker::operatorSeekTransactionTimesOutWhenSeekGenerationUncommitted() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(5000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(1000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);

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

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(60, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    const PlaybackWorker::OperatorSeekResult result = worker.seekToAndWaitForPgm(5000, 1, 5);

    QVERIFY(!result.completed);
    QVERIFY(!result.submittedPgm);
    QVERIFY(result.timedOut);
    QCOMPARE(result.targetMs, qint64(5000));
    QCOMPARE(result.generation, uint64_t(8));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
    QCOMPARE(pgmSink.frames.size(), 0);
}

void TestPlaybackWorker::operatorSeekTransactionAbandonedOnTimeout() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(5000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(1000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);

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

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(60, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    const PlaybackWorker::OperatorSeekResult result = worker.seekToAndWaitForPgm(5000, 1, 5);
    QVERIFY(result.timedOut);

    // A timed-out operator seek must abandon its transaction so the worker thread does
    // not later complete it and submit PGM for a command the caller already gave up on.
    QVERIFY(!worker.hasOperatorSeekTransaction(result.generation));
    QVERIFY(!worker.m_operatorSeekCompletion.waiting);

    // A subsequent reposition-commit completion for the abandoned generation is a no-op.
    OutputDispatchReport lateReport;
    lateReport.requiredSubmitted = true;
    worker.completeOperatorSeekTransaction(result.generation, result.targetMs, lateReport);
    QVERIFY(!worker.m_operatorSeekCompletion.completed);
}

void TestPlaybackWorker::operatorSeekCompletionEmitsSignal() {
    // Harness identical to operatorSeekTransactionTimesOutWhenSeekGenerationUncommitted
    // (tst_playbackworker.cpp:850) up to the runtime/sink setup, except the cache DOES
    // cover the target so the blocking path completes inline.
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
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

void TestPlaybackWorker::seekToWithPgmNotifyCompletesCacheHitInline() {
    // Same covered-cache harness as operatorSeekCompletionEmitsSignal, but driven
    // through the non-blocking entry point: the cache-hit clause must dispatch PGM
    // and complete the transaction inline (no worker thread running here).
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
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

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(60, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

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
    // Same harness, but the cache only covers 1000ms and the target (5000ms) is
    // uncovered: seekToWithPgmNotify must return immediately without dispatching
    // PGM, leaving the transaction registered and still waiting.
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    TestPgmSink pgmSink;
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

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(60, 1), 1, 4, 4);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{pgm, &pgmSink}});
    }

    QSignalSpy spy(&worker, &PlaybackWorker::operatorSeekCompleted);
    const quint64 generation = worker.seekToWithPgmNotify(5000, 1);
    QVERIFY(worker.hasOperatorSeekTransaction(generation)); // still waiting — no block
    QCOMPARE(pgmSink.frames.size(), 0);
    QCoreApplication::processEvents();
    QCOMPARE(spy.count(), 0); // nothing resolved yet
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

void TestPlaybackWorker::reuseAtAcceptsSelectedFeedWhenOtherFeedMissing() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_requireAllOutputFeedsForPlayhead.store(false, std::memory_order_release);

    auto* selected = new DecoderTrack;
    selected->feedIndex = 0;
    selected->buffer.insert(1000, testVideoFrame(0, 1000, 80), 8, 1000, 1000);
    worker.m_decoderBank.append(selected);

    auto* missing = new DecoderTrack;
    missing->feedIndex = 1;
    worker.m_decoderBank.append(missing);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(worker.reuseAt(1000));

    worker.m_decoderBank.clear();
    delete selected;
    delete missing;
}

void TestPlaybackWorker::reuseAtRejectsStalePriorFrameForOperatorSeek() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->buffer.insert(900, testVideoFrame(0, 900, 80), 8, 1000, 1000);
    track->buffer.insert(1100, testVideoFrame(0, 1100, 90), 8, 1000, 1100);
    worker.m_decoderBank.append(track);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 900, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1100, 90));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(worker
                .outputFeedCoverageInCache(*worker.m_outputCache, 0, 1000, 0,
                                           PlaybackWorker::OutputCoverageMode::Displayable)
                .has_value());
    QVERIFY(!worker.reuseAt(1000));

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::priorFrameAtFrameDurationBoundaryDoesNotCoverSeek() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(3, std::memory_order_release);
    worker.m_committedGeneration.store(3, std::memory_order_release);
    worker.m_committedPlayheadMs.store(3000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(3000, std::memory_order_release);

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->buffer.insert(3000, testVideoFrame(0, 3000, 80), 8, 3000, 3000);
    worker.m_decoderBank.append(track);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 3000, 80));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(!worker.outputFeedCoverageInCache(*worker.m_outputCache, 0, 3033, 0).has_value());

    worker.seekTo(3033, 1);
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(4));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(3));

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::stalePriorFrameDoesNotCoverFarSeek() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(900, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(900, std::memory_order_release);

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->buffer.insert(900, testVideoFrame(0, 900, 80), 8, 900, 900);
    worker.m_decoderBank.append(track);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 900, 80));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(!worker.reuseAt(5000));

    worker.seekTo(5000, 1);
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::unbracketedPriorFrameDoesNotCoverNextSourceFrame() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(12033, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(12033, std::memory_order_release);

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->buffer.insert(12033, testVideoFrame(0, 12033, 80), 8, 12033, 12033);
    worker.m_decoderBank.append(track);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 12033, 80));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(!worker.reuseAt(12100));

    worker.seekTo(12100, 1);
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::sameCadenceFutureBeyondRoundingWindowDoesNotCoverMissingFrame() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(testVideoFrame(0, 11966, 80));
    cache.insertVideoFrame(testVideoFrame(0, 12033, 90));

    QVERIFY(!worker.outputFeedCoverageInCache(cache, 0, 12000, 0).has_value());
}

void TestPlaybackWorker::sameCadenceGapNearFutureDoesNotCoverMissingFrame() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(testVideoFrame(0, 28033, 80));
    cache.insertVideoFrame(testVideoFrame(0, 28400, 90));

    QVERIFY(!worker.outputFeedCoverageInCache(cache, 0, 28366, 0).has_value());
    QVERIFY(!worker
                 .outputFeedCoverageInCache(cache, 0, 28366, 0,
                                            PlaybackWorker::OutputCoverageMode::Displayable)
                 .has_value());
}

void TestPlaybackWorker::staleNearFutureSeekDoesNotCommitFromCache() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);
    transport.seek(28366);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(28400, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(28400, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 28033, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 28400, 90));
        worker.publishOutputCacheLocked();
    }

    worker.seekTo(28366, -1);

    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(28400));
}

void TestPlaybackWorker::operatorSeekRejectsStaleBracketedCacheCoverage() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);
    transport.seek(27933);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(28000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(28000, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 27733, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 28000, 90));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(!worker
                 .outputFeedCoverageInCache(*worker.m_outputCache, 0, 27933, 0,
                                            PlaybackWorker::OutputCoverageMode::OperatorSeek)
                 .has_value());
    QVERIFY(worker
                .outputFeedCoverageInCache(*worker.m_outputCache, 0, 27933, 0,
                                           PlaybackWorker::OutputCoverageMode::Displayable)
                .has_value());

    worker.seekTo(27933, -1);

    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(8));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(7));
    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(28000));
}

void TestPlaybackWorker::operatorTransactionDisablesDisplayableRepositionFallback() {
    FrameProvider feed0;
    PlaybackTransport transport;
    PlaybackWorker worker({&feed0}, &transport);

    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_operatorSeekCompletion = PlaybackWorker::OperatorSeekCompletionState{};
        worker.m_operatorSeekCompletion.waiting = true;
        worker.m_operatorSeekCompletion.completed = false;
        worker.m_operatorSeekCompletion.generation = 42;
        worker.m_operatorSeekCompletion.targetMs = 27933;
    }

    QVERIFY(!worker.allowDisplayableFallbackForReposition(42));
    QVERIFY(worker.allowDisplayableFallbackForReposition(41));

    {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_operatorSeekCompletion.completed = true;
    }
    QVERIFY(worker.allowDisplayableFallbackForReposition(42));
}

void TestPlaybackWorker::liveStartupSeekCommitsFirstDisplayableFrameWhenZeroIsUncovered() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);
    transport.seek(0);
    transport.setPlaying(true);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(4, std::memory_order_release);
    worker.m_committedGeneration.store(4, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 832, 80));
        worker.publishOutputCacheLocked();
    }

    worker.seekTo(0, 1);

    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(5));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(5));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(832));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(832));
}

void TestPlaybackWorker::lowerCadenceDisplayableHoldBoundaries() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    OutputFrameCache acceptedBoundary(1, 4, 4);
    acceptedBoundary.insertVideoFrame(testVideoFrame(0, 1000, 80));
    acceptedBoundary.insertVideoFrame(testVideoFrame(0, 1233, 90));
    QVERIFY(worker
                .outputFeedCoverageInCache(acceptedBoundary, 0, 1200, 0,
                                           PlaybackWorker::OutputCoverageMode::Displayable)
                .has_value());

    OutputFrameCache rejectedBoundary(1, 4, 4);
    rejectedBoundary.insertVideoFrame(testVideoFrame(0, 1000, 80));
    rejectedBoundary.insertVideoFrame(testVideoFrame(0, 1234, 90));
    QVERIFY(!worker
                 .outputFeedCoverageInCache(rejectedBoundary, 0, 1201, 0,
                                            PlaybackWorker::OutputCoverageMode::Displayable)
                 .has_value());

    OutputFrameCache widerFutureGap(1, 4, 4);
    widerFutureGap.insertVideoFrame(testVideoFrame(0, 1000, 80));
    widerFutureGap.insertVideoFrame(testVideoFrame(0, 1237, 90));
    QVERIFY(worker
                .outputFeedCoverageInCache(widerFutureGap, 0, 1201, 0,
                                           PlaybackWorker::OutputCoverageMode::Displayable)
                .has_value());
}

void TestPlaybackWorker::strictSeekNeedsPriorFrameWhenFutureIsOneTickAhead() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(testVideoFrame(0, 30000, 80));

    QVERIFY(!worker.outputFeedCoverageInCache(cache, 0, 29980, 0).has_value());
    QVERIFY(worker
                .outputFeedCoverageInCache(cache, 0, 29980, 0,
                                           PlaybackWorker::OutputCoverageMode::Displayable)
                .has_value());
}

void TestPlaybackWorker::staleExactGpuFrameCannotBeCoveredByFreshNeighbors() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(testVideoFrame(0, 11966, 80));
    FrameHandle staleExact = testVideoFrame(0, 12000, 90);
    staleExact.metadata().gpuGeneration = 1;
    cache.insertVideoFrame(staleExact);
    cache.insertVideoFrame(testVideoFrame(0, 12033, 100));

    QVERIFY(!worker.outputFeedCoverageInCache(cache, 0, 12000, 2).has_value());
}

void TestPlaybackWorker::uncoveredEarlySeekFindsFirstDisplayableCachePlayhead() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_requireAllOutputFeedsForPlayhead.store(true, std::memory_order_release);

    QMutexLocker bufferLocker(&worker.m_bufferMutex);
    worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
    worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 800, 80));
    worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 960, 120));

    const std::optional<qint64> displayable = worker.outputCacheDisplayablePlayheadLocked(7, 0);
    QVERIFY(displayable.has_value());
    QCOMPARE(*displayable, qint64(960));
    QVERIFY(worker.m_outputCache->videoFrameAtFreshForGeneration(0, *displayable, 0).has_value());
    QVERIFY(worker.m_outputCache->videoFrameAtFreshForGeneration(1, *displayable, 0).has_value());
}

void TestPlaybackWorker::uncoveredTailSeekFindsLastDisplayableCachePlayhead() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);

    QMutexLocker bufferLocker(&worker.m_bufferMutex);
    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
    worker.publishOutputCacheLocked();

    const std::optional<qint64> displayable = worker.outputCacheDisplayablePlayheadLocked(1180, 0);
    QVERIFY(displayable.has_value());
    QCOMPARE(*displayable, qint64(1180));

    const std::optional<qint64> tooFar = worker.outputCacheDisplayablePlayheadLocked(3000, 0);
    QVERIFY(!tooFar.has_value());
}

void TestPlaybackWorker::outputSnapshotIgnoresUncoveredBookmarkAndSnapsToDisplayableCache() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);
    transport.seek(1300);
    transport.setPlaying(true);

    PlaybackWorker worker({&feed0}, &transport);
    worker.initializeOutputGraph(1, 4, 4);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1400, 80));
        worker.publishOutputCacheLocked();
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1400));
    QVERIFY(snapshot.cache
                .videoFrameAtFreshForGeneration(0, snapshot.state.playheadMs,
                                                snapshot.state.gpuGeneration)
                .has_value());
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(1400));
}

void TestPlaybackWorker::outputSnapshotAdvancesPastCoveredBookmarkToLiveTailFrame() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);
    transport.seek(12000);
    transport.setPlaying(true);

    PlaybackWorker worker({&feed0}, &transport);
    worker.initializeOutputGraph(1, 4, 4);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(900, std::memory_order_release);
    worker.m_outputPlayheadCacheGuarded.store(true, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 900, 60));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 11950, 100));
        worker.publishOutputCacheLocked();
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(12000));
    const std::optional<FrameHandle> frame = snapshot.cache.videoFrameAtFreshForGeneration(
        0, snapshot.state.playheadMs, snapshot.state.gpuGeneration);
    QVERIFY(frame.has_value());
    QCOMPARE(frame->metadata().key.ptsMs, qint64(11950));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(12000));
}

void TestPlaybackWorker::pausedWorkerKeepsWorkingUntilOutputCacheCoversPlayhead() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);
    transport.seek(1000);
    transport.setPlaying(false);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_requireAllOutputFeedsForPlayhead.store(true, std::memory_order_release);

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->lastDeliveredPtsMs = 1000;
    track->buffer.insert(1000, testVideoFrame(0, 1000, 80), 8, 1000, 1000);
    worker.m_decoderBank.append(track);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
        worker.publishOutputCacheLocked();
    }

    QVERIFY(worker.pausedPlayheadNeedsWork(1000));

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::multiviewPausedCoverageAllowsBracketedLowerCadenceFrames() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(50, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_requireAllOutputFeedsForPlayhead.store(true, std::memory_order_release);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 960, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1040, 90));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 900, 120));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 1100, 130));
        worker.publishOutputCacheLocked();
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    QVERIFY(!snapshot.state.forcePlayEpochReset);
}

void TestPlaybackWorker::liveDisplayableCoverageAllowsLowerCadenceHoldNearFuture() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.setFrameRate(60, 1);
    transport.seek(17);

    PlaybackWorker worker({&feed0}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 0, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 33, 90));
        worker.publishOutputCacheLocked();
    }

    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(17));
    QVERIFY(!snapshot.state.forcePlayEpochReset);
}

void TestPlaybackWorker::multiviewCoverageHoldsPlayheadWhenAnyFeedMissing() {
    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.m_outputFeedCount = 2;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_requireAllOutputFeedsForPlayhead.store(true, std::memory_order_release);
    worker.m_seekGeneration.store(7, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);
    worker.m_committedPlayheadMs.store(500, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(500, std::memory_order_release);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(2, 4, 4);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 80));
        worker.publishOutputCacheLocked();
    }

    OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(500));
    QVERIFY(snapshot.state.forcePlayEpochReset);

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 1000, 120));
        worker.publishOutputCacheLocked();
    }

    snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
}

void TestPlaybackWorker::seekDirectionHintSurvivesUpdatedTransport() {
    FrameProvider feed0;
    PlaybackTransport transport;
    transport.seek(960);

    PlaybackWorker worker({&feed0}, &transport);
    worker.seekTo(960, -1);

    QCOMPARE(worker.m_lastMoveDir.load(std::memory_order_relaxed), -1);
}

void TestPlaybackWorker::primaryVideoPacketIndexingDoesNotRequireCodecContext() {
    FrameProvider feed0;
    PlaybackTransport transport;
    PlaybackWorker worker({&feed0}, &transport);

    auto* primary = new DecoderTrack;
    primary->streamIndex = 7;
    primary->feedIndex = 0;
    primary->codecCtx = nullptr;
    worker.m_decoderBank.append(primary);

    AVPacket* pkt = av_packet_alloc();
    QVERIFY(pkt != nullptr);
    pkt->stream_index = 7;
    pkt->pos = 123456;

    worker.indexPrimaryVideoPacketForSeek(primary, pkt, 2400);

    QCOMPARE(worker.m_frameIndex.size(), 1);
    const std::optional<qint64> offset = worker.m_frameIndex.nearestAtOrBefore(2400);
    QVERIFY(offset.has_value());
    QCOMPARE(offset.value(), qint64(123456));

    av_packet_free(&pkt);
    worker.m_decoderBank.clear();
    delete primary;
}

#ifdef OLR_GPU_PIPELINE_BUILD
void TestPlaybackWorker::gpuBudgetConfiguredFromOutputGraphGeometry() {
    qputenv("OLR_GPU_PIPELINE", "1");

    std::vector<std::unique_ptr<FrameProvider>> feedStorage;
    QList<FrameProvider*> feeds;
    for (int i = 0; i < 8; ++i) {
        feedStorage.push_back(std::make_unique<FrameProvider>());
        feeds.append(feedStorage.back().get());
    }
    FrameProvider multiviewProvider;
    FrameProvider pgmProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    GpuBudgetConfig zero;
    zero.aggregateDecodeWindow = 0;
    zero.stagingWindowPerFeed = 0;
    zero.activeBusCount = 0;
    zero.readbackRingDepth = 0;
    GpuBudget::instance().configure(zero);
    GpuBudget::instance().reset();

    PlaybackWorker worker(feeds, &transport);
    worker.setBusPreviewProviders(&multiviewProvider, &pgmProvider);
    worker.initializeOutputGraph(8, 1280, 720);

    GpuBudgetConfig expected;
    expected.feedCount = 8;
    expected.aggregateDecodeWindow = 256;
    expected.stagingWindowPerFeed = 20; // ceil(800 ms / 40 ms at 25 fps)
    expected.activeBusCount = 10;       // one feed bus per feed + PGM + multiview
    expected.readbackRingDepth = 3;
    expected.width = 1280;
    expected.height = 720;
    expected.surfaceFormat = FramePixelFormat::Nv12;

    GpuBudgetConfig oneFeed = expected;
    oneFeed.feedCount = 1;
    oneFeed.activeBusCount = 3;

    QCOMPARE(GpuBudget::instance().budgetBytes(), expected.peakBudgetBytes());
    QVERIFY(expected.peakBudgetBytes() > oneFeed.peakBudgetBytes());
}

void TestPlaybackWorker::gpuBudgetUsesCodecGeometryForDecodeSurfaces() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    GpuBudget::instance().reset();

    PlaybackWorker worker({&feedProvider}, &transport);
    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->codecWidth = 3840;
    track->codecHeight = 2160;
    worker.m_decoderBank.append(track);

    worker.initializeOutputGraph(1, 1280, 720);

    GpuBudgetConfig expected;
    expected.feedCount = 1;
    expected.aggregateDecodeWindow = 256;
    expected.stagingWindowPerFeed = 20;
    expected.activeBusCount = 1;
    expected.readbackRingDepth = 3;
    expected.width = 1280;
    expected.height = 720;
    expected.surfaceWidth = 3840;
    expected.surfaceHeight = 2160;
    expected.outputWidth = 1280;
    expected.outputHeight = 720;
    expected.readbackWidth = 1280;
    expected.readbackHeight = 720;
    expected.surfaceFormat = FramePixelFormat::Nv12;

    QCOMPARE(GpuBudget::instance().budgetBytes(), expected.peakBudgetBytes());

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::gpuForceBudgetConstrainsConfiguredBudget() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_GPU_FORCE_BUDGET", "12");

    std::vector<std::unique_ptr<FrameProvider>> feedStorage;
    QList<FrameProvider*> feeds;
    for (int i = 0; i < 4; ++i) {
        feedStorage.push_back(std::make_unique<FrameProvider>());
        feeds.append(feedStorage.back().get());
    }
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    GpuBudget::instance().reset();

    PlaybackWorker worker(feeds, &transport);
    worker.initializeOutputGraph(4, 64, 48);

    GpuBudgetConfig expected;
    expected.feedCount = 4;
    expected.aggregateDecodeWindow = 48; // OLR_GPU_FORCE_BUDGET is per track
    expected.stagingWindowPerFeed = 0;
    expected.activeBusCount = 4;
    expected.readbackRingDepth = 3;
    expected.width = 64;
    expected.height = 48;
    expected.outputWidth = 64;
    expected.outputHeight = 48;
    expected.readbackWidth = 64;
    expected.readbackHeight = 48;
    expected.surfaceFormat = FramePixelFormat::Nv12;

    QCOMPARE(GpuBudget::instance().budgetBytes(), expected.peakBudgetBytes());
}

void TestPlaybackWorker::residencyWindowParamsPreserveDefaultCap() {
    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);

    QCOMPARE(worker.capFrames(4), 47);
}

void TestPlaybackWorker::residencyWindowParamsCanExpandTrailCap() {
    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(30, 1);
    PlaybackWorker worker({&feedProvider}, &transport);

    PlaybackWorker::ResidencyWindowParams params;
    params.leadMs = 500;
    params.trailMs = 2000;
    params.chunkMs = 500;
    params.slackMs = 200;
    params.globalFrameBudget = 512;
    params.perTrackCapOverride = 128;
    worker.setResidencyWindowParamsForTest(params);

    QVERIFY(worker.capFrames(4) > 50);
    QCOMPARE(worker.capFrames(4), 108);
}

void TestPlaybackWorker::outputStatsSurfaceGpuBudgetCounters() {
    qputenv("OLR_GPU_PIPELINE", "1");
    qputenv("OLR_LEDGER_REPORT_ONLY", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);

    GpuBudget::instance().reset();
    GpuBudgetCharge gatedCharge(2000000, GpuBudgetTag::DecodeWindow);
    GpuBudgetCharge chargeOnlyCharge(1110400, GpuBudgetTag::IngestWrap);
    GpuBudget::instance().noteOomDegrade();
    GpuBudget::instance().noteOomDegrade();

    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);

    const OutputDispatchStats stats = worker.outputStats();
    QCOMPARE(stats.gpuVramBytes, qint64(3110400));
    QCOMPARE(stats.gpuBudgetBytes, GpuBudget::instance().budgetBytes());
    QCOMPARE(stats.gpuGatedLiveBytes, qint64(2000000));
    QVERIFY(stats.gpuBudgetReportOnly);
    QCOMPARE(stats.gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::DecodeWindow)],
             qint64(2000000));
    QCOMPARE(stats.gpuLiveBytesByTag[static_cast<int>(GpuBudgetTag::IngestWrap)], qint64(1110400));
    QCOMPARE(stats.gpuOomDegrades, qint64(2));
    QCOMPARE(stats.gpuDeviceLossEvents, qint64(0));

    qunsetenv("OLR_GPU_PIPELINE");
    const OutputDispatchStats disabledStats = worker.outputStats();
    QCOMPARE(disabledStats.gpuVramBytes, qint64(0));
    QCOMPARE(disabledStats.gpuBudgetBytes, qint64(0));
    QCOMPARE(disabledStats.gpuGatedLiveBytes, qint64(0));
    QVERIFY(!disabledStats.gpuBudgetReportOnly);
    for (qint64 taggedBytes : disabledStats.gpuLiveBytesByTag)
        QCOMPARE(taggedBytes, qint64(0));
    QCOMPARE(disabledStats.gpuOomDegrades, qint64(0));
    QCOMPARE(disabledStats.gpuDeviceLossEvents, qint64(0));
}

void TestPlaybackWorker::gpuSeekPrefetchPlanUsesBudgetHeadroom() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);

    GpuBudgetConfig cfg;
    cfg.feedCount = 1;
    cfg.aggregateDecodeWindow = 4;
    cfg.stagingWindowPerFeed = 0;
    cfg.activeBusCount = 0;
    cfg.readbackRingDepth = 0;
    cfg.width = 64;
    cfg.height = 48;
    cfg.surfaceFormat = FramePixelFormat::Nv12;
    GpuBudget::instance().configure(cfg);
    GpuBudget::instance().reset();
    GpuBudgetCharge liveCharge(cfg.surfaceBytes() * 2);

    const auto plan = worker.planGpuSeekPrefetchForReposition(1000, 1);
    QCOMPARE(plan.startMs, int64_t(1000));
    QCOMPARE(plan.endMs, int64_t(1500));
    QCOMPARE(plan.surfaceCount, 2);

    const PlaybackWorker::PlaybackCounters counters = worker.counters();
    QCOMPARE(counters.gpuSeekPrefetchConsults, qint64(1));
    QCOMPARE(counters.gpuSeekPrefetchPlannedSurfaces, qint64(2));

    qunsetenv("OLR_GPU_PIPELINE");
    const auto disabledPlan = worker.planGpuSeekPrefetchForReposition(1000, 1);
    QCOMPARE(disabledPlan.surfaceCount, 0);
    const PlaybackWorker::PlaybackCounters disabledCounters = worker.counters();
    QCOMPARE(disabledCounters.gpuSeekPrefetchConsults, qint64(1));
    QCOMPARE(disabledCounters.gpuSeekPrefetchPlannedSurfaces, qint64(2));
}

void TestPlaybackWorker::gpuSeekPrefetchPlanUsesCodecGeometryForSurfaceBytes() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    track->codecWidth = 128;
    track->codecHeight = 64;
    worker.m_decoderBank.append(track);
    worker.initializeOutputGraph(1, 64, 48);

    GpuBudgetConfig cfg;
    cfg.feedCount = 1;
    cfg.aggregateDecodeWindow = 2;
    cfg.stagingWindowPerFeed = 0;
    cfg.activeBusCount = 0;
    cfg.readbackRingDepth = 0;
    cfg.width = 64;
    cfg.height = 48;
    cfg.surfaceWidth = 128;
    cfg.surfaceHeight = 64;
    cfg.surfaceFormat = FramePixelFormat::Nv12;
    GpuBudget::instance().configure(cfg);
    GpuBudget::instance().reset();

    const auto plan = worker.planGpuSeekPrefetchForReposition(1000, 1);
    QCOMPARE(plan.surfaceCount, 2);

    worker.m_decoderBank.clear();
    delete track;
}

void TestPlaybackWorker::gpuSeekPrefetchAllowanceBoundsNativeGpuDecode() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);

    GpuBudgetConfig cfg;
    cfg.feedCount = 1;
    cfg.aggregateDecodeWindow = 2;
    cfg.stagingWindowPerFeed = 0;
    cfg.activeBusCount = 0;
    cfg.readbackRingDepth = 0;
    cfg.width = 64;
    cfg.height = 48;
    cfg.surfaceFormat = FramePixelFormat::Nv12;
    GpuBudget::instance().configure(cfg);
    GpuBudget::instance().reset();

    const auto plan = worker.beginGpuSeekPrefetchForReposition(1000, 1);
    QCOMPARE(plan.surfaceCount, 2);
    QVERIFY(worker.allowNativeGpuDecodeForCurrentPacket(1000));
    QVERIFY(worker.allowNativeGpuDecodeForCurrentPacket(1040));
    QVERIFY(!worker.allowNativeGpuDecodeForCurrentPacket(1080));

    PlaybackWorker::PlaybackCounters counters = worker.counters();
    QCOMPARE(counters.gpuSeekPrefetchGpuAttempts, qint64(2));

    worker.endGpuSeekPrefetchForReposition();
    QVERIFY(worker.allowNativeGpuDecodeForCurrentPacket(900));
    counters = worker.counters();
    QCOMPARE(counters.gpuSeekPrefetchGpuAttempts, qint64(2));
}

void TestPlaybackWorker::gpuSeekPrefetchAllowanceUsesPlannedWindow() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);

    GpuBudgetConfig cfg;
    cfg.feedCount = 1;
    cfg.aggregateDecodeWindow = 3;
    cfg.stagingWindowPerFeed = 0;
    cfg.activeBusCount = 0;
    cfg.readbackRingDepth = 0;
    cfg.width = 64;
    cfg.height = 48;
    cfg.surfaceFormat = FramePixelFormat::Nv12;
    GpuBudget::instance().configure(cfg);
    GpuBudget::instance().reset();

    const auto plan = worker.beginGpuSeekPrefetchForReposition(1000, 1);
    QCOMPARE(plan.startMs, int64_t(1000));
    QCOMPARE(plan.endMs, int64_t(1500));
    QCOMPARE(plan.surfaceCount, 3);

    QVERIFY(!worker.allowNativeGpuDecodeForCurrentPacket(700));
    QVERIFY(worker.allowNativeGpuDecodeForCurrentPacket(1000));
    QVERIFY(worker.allowNativeGpuDecodeForCurrentPacket(1200));
    QVERIFY(worker.allowNativeGpuDecodeForCurrentPacket(1500));
    QVERIFY(!worker.allowNativeGpuDecodeForCurrentPacket(1520));

    const PlaybackWorker::PlaybackCounters counters = worker.counters();
    QCOMPARE(counters.gpuSeekPrefetchGpuAttempts, qint64(3));
}

void TestPlaybackWorker::gpuSeekPrefetchDoesNotExtendManualSeekCommitFill() {
    GpuPrefetchPlan plan;
    plan.startMs = 1000;
    plan.endMs = 1500;
    plan.surfaceCount = 3;

    QCOMPARE(PlaybackWorker::manualSeekCommitFillToForTest(1000, 40, plan), int64_t(1040));
}

void TestPlaybackWorker::gpuPressureDerivesRuntimeBudgetFromHeadroom() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);

    GpuBudget::instance().reset();
    const qint64 twoGiB = qint64(2) * 1024 * 1024 * 1024;
    worker.evaluateGpuMemoryPressureForTest(uint64_t(twoGiB), false, 1000);

    QCOMPARE(GpuBudget::instance().budgetBytes(), twoGiB / 2);
    QCOMPARE(worker.counters().gpuMemoryPressureLevel1, qint64(0));
}

void TestPlaybackWorker::gpuPressureWarningShrinksWindowWithoutLatch() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);

    PlaybackWorker::ResidencyWindowParams params;
    params.trailMs = 2000;
    params.globalFrameBudget = 512;
    params.perTrackCapOverride = 128;
    worker.setResidencyWindowParamsForTest(params);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    worker.evaluateGpuMemoryPressureForTest(0, true, 1000);

    QCOMPARE(worker.m_residencyWindowParams.trailMs, 1000);
    QCOMPARE(worker.counters().gpuMemoryPressureLevel1, qint64(1));
    QCOMPARE(worker.counters().gpuMemoryPressureLevel2, qint64(0));
    QVERIFY(!worker.m_memoryPressureLatched.load(std::memory_order_acquire));
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
}

void TestPlaybackWorker::gpuPressureBelowLevel1LatchesCpuBeforeJetsam() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    worker.evaluateGpuMemoryPressureForTest(192 * 1024 * 1024, false, 1000);

    QVERIFY(worker.m_memoryPressureLatched.load(std::memory_order_acquire));
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(worker.counters().gpuMemoryPressureLevel1, qint64(1));
    QCOMPARE(worker.counters().gpuMemoryPressureLevel2, qint64(1));
}

void TestPlaybackWorker::gpuPressureLevel2LatchesCpuWithoutDeviceLoss() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    worker.m_gpuDeviceLossRebuildsRemaining.store(3, std::memory_order_release);
    GpuDeviceLossMonitor::instance().reset();

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);

    QVERIFY(worker.m_memoryPressureLatched.load(std::memory_order_acquire));
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(worker.m_gpuDeviceLossRebuildsRemaining.load(std::memory_order_acquire), 3);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
    QCOMPARE(worker.counters().gpuMemoryPressureLevel1, qint64(1));
    QCOMPARE(worker.counters().gpuMemoryPressureLevel2, qint64(1));
}

void TestPlaybackWorker::gpuPressureRecoveryReanchorsCommittedPlayheadDuringPendingSeek() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);

    TestPgmSink sink;
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 72));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);

    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(2));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(1));
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire),
             GpuGenerationCounter::instance().current());
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setEndpoints({{feedAssignment(OutputTargetKind::Ndi), &sink}});
    }
    worker.m_outputRuntime->dispatchImmediate();
    QCOMPARE(sink.frames.size(), 1);
    const OutputFrameIdentity identity = sink.frames.constFirst().identity;
    QCOMPARE(identity.sampledPlayheadMs, qint64(1000));
    QCOMPARE(identity.sourcePtsMs, qint64(1000));
    QVERIFY(!identity.videoPlaceholder);
    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    QCOMPARE(snapshot.cache
                 .videoFrameAtFreshForGeneration(0, snapshot.state.playheadMs,
                                                 snapshot.state.gpuGeneration)
                 .has_value(),
             true);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->setEndpoints({});
    }
}

void TestPlaybackWorker::gpuPressureRecoveryHonorsSelectedFeedMode() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feed0;
    FrameProvider feed1;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);

    PlaybackWorker worker({&feed0, &feed1}, &transport);
    worker.initializeOutputGraph(2, 64, 48);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_requireAllOutputFeedsForPlayhead.store(false, std::memory_order_release);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1000, 88));
        worker.publishOutputCacheLocked();
    }

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);

    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(1000));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(1000));
    const OutputRuntimeSnapshot snapshot = worker.makeOutputSnapshot();
    QCOMPARE(snapshot.state.playheadMs, qint64(1000));
    QVERIFY(snapshot.cache
                .videoFrameAtFreshForGeneration(0, snapshot.state.playheadMs,
                                                snapshot.state.gpuGeneration)
                .has_value());
}

void TestPlaybackWorker::countersExposeGpuFallbackOutputGateState() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1234);

    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_seekGeneration.store(11, std::memory_order_release);
    worker.m_committedGeneration.store(10, std::memory_order_release);
    worker.m_committedPlayheadMs.store(880, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(860, std::memory_order_release);
    worker.m_outputPlayheadCacheGuarded.store(true, std::memory_order_release);
    worker.m_forceLiveOutputSnapshots.store(7, std::memory_order_release);
    worker.m_memoryPressureLatched.store(true, std::memory_order_release);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::CpuFallback),
                                    std::memory_order_release);
    worker.m_committedGpuGeneration.store(9, std::memory_order_release);

    const PlaybackWorker::PlaybackCounters counters = worker.counters();

    QCOMPARE(counters.transportPlayheadMs, qint64(1234));
    QCOMPARE(counters.committedPlayheadMs, qint64(880));
    QCOMPARE(counters.lastVisiblePlayheadMs, qint64(860));
    QCOMPARE(counters.seekGeneration, uint64_t(11));
    QCOMPARE(counters.committedGeneration, uint64_t(10));
    QCOMPARE(counters.committedGpuGeneration, uint64_t(9));
    QCOMPARE(counters.outputPlayheadCacheGuarded, true);
    QCOMPARE(counters.forceLiveOutputSnapshots, 7);
    QCOMPARE(counters.memoryPressureLatched, true);
    QCOMPARE(counters.gpuPipelineState,
             static_cast<int>(PlaybackWorker::GpuPipelineState::CpuFallback));
    QCOMPARE(counters.currentGpuGeneration, worker.gpuGeneration());
}

void TestPlaybackWorker::initializeOutputGraphClearsMemoryPressureLatch() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_memoryPressureLatched.store(true, std::memory_order_release);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::CpuFallback),
                                    std::memory_order_release);

    worker.initializeOutputGraph(1, 64, 48);

    QVERIFY(!worker.m_memoryPressureLatched.load(std::memory_order_acquire));
}

void TestPlaybackWorker::initializeOutputGraphCommitsPlaceholderGenerationAndEpoch() {
    qputenv("OLR_GPU_PIPELINE", "1");

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(400);
    transport.setPlaying(true);
    TestPgmSink sink;
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_seekGeneration.store(5, std::memory_order_release);
    worker.m_committedGeneration.store(5, std::memory_order_release);
    worker.m_committedPlayheadMs.store(400, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(400, std::memory_order_release);

    worker.initializeOutputGraph(1, 64, 48);

    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(5));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(5));
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(400));
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire),
             GpuGenerationCounter::instance().current());
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), 1);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setEndpoints({{feedAssignment(OutputTargetKind::Ndi), &sink}});
    }
    worker.m_outputRuntime->dispatchDueTicksForTest(0);
    QCOMPARE(sink.frames.size(), 1);
    const OutputFrameIdentity identity = sink.frames.constFirst().identity;
    QCOMPARE(identity.sampledPlayheadMs, qint64(400));
    QCOMPARE(identity.sourcePtsMs, qint64(400));
    QVERIFY(identity.videoPlaceholder);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->setEndpoints({});
    }
}
#endif

QTEST_GUILESS_MAIN(TestPlaybackWorker)
#include "tst_playbackworker.moc"
