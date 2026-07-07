#include <QtTest>

#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/qtpreviewsink.h"
#include "playback/output/queuedoutputsink.h"
#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpurhicontext.h"
#endif
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <memory>
#include <vector>

class TestPlaybackWorker : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void gpuFlagWrapsSinksInReadbackRing();
    void pgmPreviewGetsDepthOneRing();
    void buildsSinkForNewIoTargetKinds_data();
    void buildsSinkForNewIoTargetKinds();
    void newIoTargetsShareGpuReadbackCache();
    void selectedFeedCoverageAdvancesPlayheadWhenOtherFeedMissing();
    void pausedStepUsesPriorFrameCoverage();
    void coveredSeekCommitsPlayheadBeforeWorkerRuns();
    void coveredSeekPublishesLiveCacheForInstantSnapshot();
    void pausedCoveredSeekDoesNotResetOutputRuntimeSynchronously();
    void reuseAtAcceptsSelectedFeedWhenOtherFeedMissing();
    void reuseAtAcceptsPriorFrameCoverageForLowerCadenceInputs();
    void priorFrameAtFrameDurationBoundaryDoesNotCoverSeek();
    void stalePriorFrameDoesNotCoverFarSeek();
    void unbracketedPriorFrameDoesNotCoverNextSourceFrame();
    void sameCadenceFutureBeyondRoundingWindowDoesNotCoverMissingFrame();
    void staleExactGpuFrameCannotBeCoveredByFreshNeighbors();
    void pausedWorkerKeepsWorkingUntilOutputCacheCoversPlayhead();
    void multiviewPausedCoverageAllowsBracketedLowerCadenceFrames();
    void liveDisplayableCoverageAllowsLowerCadenceHoldNearFuture();
    void multiviewCoverageHoldsPlayheadWhenAnyFeedMissing();
    void seekDirectionHintSurvivesUpdatedTransport();
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
    void gpuPressureDerivesRuntimeBudgetFromHeadroom();
    void gpuPressureWarningShrinksWindowWithoutLatch();
    void gpuPressureLevel2LatchesCpuWithoutDeviceLoss();
    void initializeOutputGraphClearsMemoryPressureLatch();

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
        QCOMPARE(previewReadback->ringDepth(), 3);
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
    QCOMPARE(feedPreview->ringDepth(), 3);
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
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 960, 80));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(0, 1040, 90));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 900, 120));
        worker.m_outputCache->insertVideoFrame(testVideoFrame(1, 1100, 130));
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

void TestPlaybackWorker::pausedCoveredSeekDoesNotResetOutputRuntimeSynchronously() {
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
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(50, 1), 1, 4, 4);
    }

    int resetCount = -1;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCount = worker.m_outputRuntime->playEpochResetCountForTest();
    }
    QCOMPARE(resetCount, 0);

    worker.seekTo(1000, -1);

    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCount = worker.m_outputRuntime->playEpochResetCountForTest();
    }
    QCOMPARE(resetCount, 0);
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

void TestPlaybackWorker::reuseAtAcceptsPriorFrameCoverageForLowerCadenceInputs() {
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

    QVERIFY(worker.reuseAt(1000));

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
    expected.aggregateDecodeWindow = 12;
    expected.stagingWindowPerFeed = 0;
    expected.activeBusCount = 0;
    expected.readbackRingDepth = 0;
    expected.width = 64;
    expected.height = 48;
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
#endif

QTEST_GUILESS_MAIN(TestPlaybackWorker)
#include "tst_playbackworker.moc"
