#include <QtTest>

#include "playback/frameprovider.h"
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/iosgpulifecyclesink.h"
#include "playback/output/outputdispatcher.h"
#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/qtpreviewsink.h"
#include "playback/output/queuedoutputsink.h"
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <memory>
#include <utility>

class TestGpuDeviceLostWorker : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void lossCountReflectsRecordedEvents();
    void readbackObservedLossRecordsProcessLatch();
    void staleGenerationReadbackDoesNotRecordNewLoss();
    void lostContextRebuildsFreshGpuSpine();
    void rebuildFailureLatchesCpuFallback();
    void repeatedLossesLatchCpuFallbackAfterBudget();
    void backgroundSuspendDefersGpuRebuildUntilForeground();
    void repeatedBackgroundSuspendResumeDoesNotConsumeDeviceLossBudget();
    void lossSanitizesDecoderTrackBuffers();
    void lossRecoveryCommitsSubmittedIdentityAndEpoch();
    void armedCutDeviceLossCommitsRecoveryEpochBeforeHoldLast();
    void pendingSeekDeviceLossPublishesPlaceholderSafeRecovery();
    void pendingSeekMemoryPressurePublishesPlaceholderSafeRecovery();
    void suspendedRecoveryDispatchesCommittedCpuIdentityOnce();

private:
    std::shared_ptr<GpuRhiContext> createTestRhi() const;
    bool installTestGpuSpine(PlaybackWorker& worker) const;
};

namespace {

class TestGpuSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
    void retainUntilFenceRetired(uint64_t) override {}
    uint64_t pendingFenceValue() const override { return 0; }
};

class CachedGpuFrameData final : public IFrameData {
public:
    explicit CachedGpuFrameData(CpuPlanes planes)
        : m_planes(std::move(planes)), m_surface(std::make_shared<TestGpuSurface>()) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override {
        m_readToCpuCalls++;
        return CpuPlanes{};
    }
    CpuPlanes cachedCpuPlanes(FramePixelFormat target) const override {
        return target == m_planes.format ? m_planes : CpuPlanes{};
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return m_planes.format; }
    int readToCpuCalls() const { return m_readToCpuCalls; }

private:
    CpuPlanes m_planes;
    std::shared_ptr<TestGpuSurface> m_surface;
    mutable int m_readToCpuCalls = 0;
};

class RecordingNdiSink final : public IOutputSink {
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
        frames.append(frame);
        return true;
    }

    QVector<OutputBusFrame> frames;

private:
    bool m_active = false;
};

OutputTargetAssignment ndiFeedAssignment() {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::Ndi;
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

CpuPlanes yuvPlanes(int width, int height, uchar y, uchar u, uchar v) {
    return solidYuv420pHandle(width, height, y, u, v).readToCpu();
}

FrameHandle gpuFrame(const std::shared_ptr<GpuRhiContext>& rhi, qint64 ptsMs,
                     uint64_t gpuGeneration, qint64 decodedSequence) {
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = ptsMs;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.format = FramePixelFormat::Nv12;
    meta.gpuGeneration = gpuGeneration;
    meta.decodedSequence = decodedSequence;
    return makeGpuFrameHandle(std::make_shared<TestGpuSurface>(), rhi, meta);
}

FrameHandle placeholderFrame(qint64 ptsMs) {
    FrameHandle frame = solidYuv420pHandle(64, 48, 16, 128, 128);
    frame.metadata().key.feedIndex = 0;
    frame.metadata().key.ptsMs = ptsMs;
    frame.metadata().key.isPlaceholder = true;
    return frame;
}

} // namespace

void TestGpuDeviceLostWorker::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
    setIosGpuLifecycleSink(nullptr);
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuDeviceLostWorker::lossCountReflectsRecordedEvents() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();

    OutputDispatchStats stats;
    QCOMPARE(stats.gpuDeviceLossEvents, qint64(0));

    QCOMPARE(monitor.lossCount(), uint64_t(0));
    monitor.recordLoss();
    monitor.recordLoss();
    QCOMPARE(monitor.lossCount(), uint64_t(1));
    QVERIFY(monitor.consumeLossEvent());
    QVERIFY(!monitor.consumeLossEvent());

    monitor.clearForRebuild();
    monitor.recordLoss();
    QCOMPARE(monitor.lossCount(), uint64_t(2));
    QVERIFY(monitor.consumeLossEvent());
    QVERIFY(!monitor.consumeLossEvent());
}

std::shared_ptr<GpuRhiContext> TestGpuDeviceLostWorker::createTestRhi() const {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) rhi = GpuRhiContext::createWarpForTest();
    return rhi;
}

bool TestGpuDeviceLostWorker::installTestGpuSpine(PlaybackWorker& worker) const {
    auto rhi = createTestRhi();
    if (!rhi || !rhi->isValid()) return false;

    auto decodeFence = DecodeDoneFence::create();
    auto renderFence = rhi->createFence();
    auto stagingFence = rhi->createFence();

    worker.m_gpuRhi = std::move(rhi);
    worker.m_decodeFence = std::move(decodeFence);
    worker.m_renderFence = std::move(renderFence);
    worker.m_stagingFence = std::move(stagingFence);
    worker.m_stagedFenceValue.store(0, std::memory_order_release);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    return true;
}

void TestGpuDeviceLostWorker::readbackObservedLossRecordsProcessLatch() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    auto rhi = createTestRhi();
    if (!rhi || !rhi->isValid()) QSKIP("no test RHI backend available");

    rhi->injectDeviceLostForTest();
    const uint64_t gen0 = GpuGenerationCounter::instance().current();
    const CpuPlanes planes =
        rhi->importAndReadback(std::make_shared<TestGpuSurface>(), FramePixelFormat::Yuv420p);

    QVERIFY(!planes.isValid());
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
}

void TestGpuDeviceLostWorker::staleGenerationReadbackDoesNotRecordNewLoss() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    auto rhi = createTestRhi();
    if (!rhi || !rhi->isValid()) QSKIP("no test RHI backend available");

    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    const FrameHandle stale = makeGpuFrameHandle(std::make_shared<TestGpuSurface>(), rhi, meta);

    rhi->injectDeviceLostForTest();
    GpuDeviceLossMonitor::instance().recordLoss();
    GpuDeviceLossMonitor::instance().clearForRebuild();
    const uint64_t stableGeneration = GpuGenerationCounter::instance().current();
    const uint64_t stableLossCount = GpuDeviceLossMonitor::instance().lossCount();

    const CpuPlanes planes = stale.readToCpu(FramePixelFormat::Yuv420p);

    QVERIFY(!planes.isValid());
    QCOMPARE(GpuGenerationCounter::instance().current(), stableGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), stableLossCount);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
}

void TestGpuDeviceLostWorker::lostContextRebuildsFreshGpuSpine() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setFeedPreviewProvidersEnabled(true);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");
    QVERIFY(worker.gpuPathActive());
    QVERIFY(worker.m_gpuRhi);
    QVERIFY(worker.m_decodeFence);
#ifdef _WIN32
    // The Windows render/staging fences are created lazily from the Media
    // Foundation import device, not from the QRhi spine.
    QVERIFY(!worker.m_renderFence);
    QVERIFY(!worker.m_stagingFence);
#else
    QVERIFY(worker.m_renderFence);
    QVERIFY(worker.m_stagingFence);
#endif

    auto lostRhi = worker.m_gpuRhi;
    lostRhi->injectDeviceLostForTest();
    QVERIFY(lostRhi->deviceLost());
    const uint64_t gen0 = GpuGenerationCounter::instance().current();

    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
    QVERIFY(worker.gpuPathActive());
    QVERIFY(worker.m_gpuRhi);
    QVERIFY(worker.m_gpuRhi != lostRhi);
    QVERIFY(!worker.m_gpuRhi->deviceLost());
    QVERIFY(worker.m_decodeFence);
#ifdef _WIN32
    QVERIFY(!worker.m_renderFence);
    QVERIFY(!worker.m_stagingFence);
#else
    QVERIFY(worker.m_renderFence);
    QVERIFY(worker.m_stagingFence);
#endif
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
    const OutputDispatchStats stats = worker.outputStats();
    QCOMPARE(stats.gpuDeviceLossEvents, qint64(1));
    QVERIFY(!GpuDeviceLossMonitor::instance().consumeLossEvent());
}

void TestGpuDeviceLostWorker::rebuildFailureLatchesCpuFallback() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setFeedPreviewProvidersEnabled(true);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    QVERIFY(installTestGpuSpine(worker));
    worker.rebuildOutputEndpoints();

    auto lostRhi = worker.m_gpuRhi;
    lostRhi->injectDeviceLostForTest();
    const uint64_t gen0 = GpuGenerationCounter::instance().current();
    qunsetenv("OLR_GPU_PIPELINE");

    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QVERIFY(!worker.gpuPathActive());
    QVERIFY(!worker.m_gpuRhi);
    QVERIFY(!worker.m_decodeFence);
    QVERIFY(!worker.m_renderFence);
    QVERIFY(!worker.m_stagingFence);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > gen0);
    const OutputDispatchStats lossStats = worker.outputStats();
    QCOMPARE(lossStats.gpuDeviceLossEvents, qint64(1));
    QCOMPARE(worker.outputStats().gpuDeviceLossEvents, qint64(1));
    QVERIFY(!GpuDeviceLossMonitor::instance().consumeLossEvent());

    qputenv("OLR_GPU_PIPELINE", "1");
    worker.rebuildOutputEndpoints();
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

    const uint64_t stableGeneration = GpuGenerationCounter::instance().current();
    const uint64_t stableLossCount = GpuDeviceLossMonitor::instance().lossCount();
    worker.handleGpuDeviceLoss();
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(GpuGenerationCounter::instance().current(), stableGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), stableLossCount);
}

void TestGpuDeviceLostWorker::repeatedLossesLatchCpuFallbackAfterBudget() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");

    for (int i = 0; i < PlaybackWorker::kDeviceLossRebuildBudget; ++i) {
        QVERIFY(worker.m_gpuRhi);
        worker.m_gpuRhi->injectDeviceLostForTest();
        worker.handleGpuDeviceLoss();
        QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
        QVERIFY(worker.gpuPathActive());
    }

    QVERIFY(worker.m_gpuRhi);
    worker.m_gpuRhi->injectDeviceLostForTest();
    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QVERIFY(!worker.gpuPathActive());
    QVERIFY(!worker.m_gpuRhi);
}

void TestGpuDeviceLostWorker::backgroundSuspendDefersGpuRebuildUntilForeground() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");
    QVERIFY(worker.m_gpuRhi);

    lifecycle.onEnterBackground();
    worker.handleGpuDeviceLoss();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);
    QVERIFY(!worker.m_gpuRhi);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());

    lifecycle.onEnterForeground();
    worker.resumeDeferredGpuRebuild();

    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
    QVERIFY(worker.gpuPathActive());
    QVERIFY(worker.m_gpuRhi);
    QCOMPARE(worker.m_gpuDeviceLossRebuildsRemaining.load(std::memory_order_acquire),
             PlaybackWorker::kDeviceLossRebuildBudget);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
}

void TestGpuDeviceLostWorker::repeatedBackgroundSuspendResumeDoesNotConsumeDeviceLossBudget() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.setExternalOutputTargets({ndiFeedAssignment()});
    worker.initializeOutputGraph(1, 64, 48);
    if (worker.gpuPipelineState() != PlaybackWorker::GpuPipelineState::Gpu)
        QSKIP("no GPU RHI backend available");

    for (int i = 0; i < PlaybackWorker::kDeviceLossRebuildBudget + 1; ++i) {
        lifecycle.onEnterBackground();
        worker.handleGpuDeviceLoss();
        QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::RebuildPending);

        lifecycle.onEnterForeground();
        worker.resumeDeferredGpuRebuild();

        QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::Gpu);
        QVERIFY(worker.gpuPathActive());
        QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
        QCOMPARE(worker.m_gpuDeviceLossRebuildsRemaining.load(std::memory_order_acquire),
                 PlaybackWorker::kDeviceLossRebuildBudget);
    }
}

void TestGpuDeviceLostWorker::lossSanitizesDecoderTrackBuffers() {
    qunsetenv("OLR_GPU_PIPELINE");
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    auto* track = new DecoderTrack;
    track->feedIndex = 0;
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.width = 16;
    meta.key.height = 16;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    meta.decodedSequence = 7;
    const auto data = std::make_shared<CachedGpuFrameData>(yuvPlanes(16, 16, 80, 90, 100));
    QVERIFY(track->buffer.insert(100, FrameHandle(data, meta), 10, 0, 1000));
    worker.m_decoderBank.append(track);

    worker.handleGpuDeviceLoss();

    FrameHandle recovered;
    int64_t pts = -1;
    QVERIFY(track->buffer.frameAt(100, recovered, pts));
    QCOMPARE(pts, int64_t(100));
    QVERIFY(!recovered.isGpuBacked());
    QCOMPARE(recovered.metadata().decodedSequence, qint64(7));
    QCOMPARE(recovered.metadata().key.ptsMs, qint64(100));
    QCOMPARE(data->readToCpuCalls(), 0);

    worker.m_decoderBank.clear();
    delete track;
}

void TestGpuDeviceLostWorker::lossRecoveryCommitsSubmittedIdentityAndEpoch() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    RecordingNdiSink sink;
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    if (!installTestGpuSpine(worker)) QSKIP("no test RHI backend available");
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);

    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    meta.decodedSequence = 17;
    const auto data = std::make_shared<CachedGpuFrameData>(yuvPlanes(64, 48, 72, 90, 110));
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(FrameHandle(data, meta));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.m_gpuRhi->injectDeviceLostForTest();
    worker.handleGpuDeviceLoss();

    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        const std::optional<FrameHandle> recovered = worker.m_outputCache->videoFrameAt(0, 100);
        QVERIFY(recovered.has_value());
        QVERIFY(!recovered->isGpuBacked());
        QVERIFY(worker.recoveredCachePlayheadLocked(100, GpuGenerationCounter::instance().current())
                    .has_value());
    }
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(100));
    QCOMPARE(worker.m_lastVisiblePlayheadMs.load(std::memory_order_acquire), qint64(100));
    QCOMPARE(worker.m_seekGeneration.load(std::memory_order_acquire), uint64_t(2));
    QCOMPARE(worker.m_committedGeneration.load(std::memory_order_acquire), uint64_t(1));
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire),
             GpuGenerationCounter::instance().current());
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
        worker.m_outputRuntime->resetFrameIndex(1000000);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setEndpoints({{ndiFeedAssignment(), &sink}});
    }
    worker.m_outputRuntime->dispatchImmediate();
    QCOMPARE(sink.frames.size(), 1);
    const OutputFrameIdentity identity = sink.frames.constFirst().identity;
    QCOMPARE(identity.sampledPlayheadMs, qint64(100));
    QCOMPARE(identity.sourcePtsMs, qint64(100));
    QVERIFY(!identity.videoPlaceholder);
    QCOMPARE(identity.videoGpuGeneration, uint64_t(0));
    QCOMPARE(identity.sourceDecodedSequence, qint64(17));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->setEndpoints({});
    }
}

void TestGpuDeviceLostWorker::armedCutDeviceLossCommitsRecoveryEpochBeforeHoldLast() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(1000);
    transport.setPlaying(true);
    RecordingNdiSink sink;
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 64;
    worker.m_outputHeight = 48;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(1000, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(1000, std::memory_order_release);
    QVERIFY(installTestGpuSpine(worker));
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 64, 48);
        worker.m_outputCache->insertVideoFrame(gpuFrame(worker.m_gpuRhi, 1000, lostGeneration, 11));
        worker.publishOutputCacheLocked();
        worker.m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 64, 48);
        worker.m_prerollStagingCache->insertVideoFrame(
            gpuFrame(worker.m_gpuRhi, 2000, lostGeneration, 22));
    }
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 64, 48);
        worker.m_outputRuntime->setIdentitySkip(false);
        worker.m_outputRuntime->setSnapshotProvider(
            [&worker]() { return worker.makeOutputSnapshot(); });
        worker.m_outputRuntime->setEndpoints({{ndiFeedAssignment(), &sink}});
    }

    worker.m_outputRuntime->dispatchImmediate();
    QCOMPARE(sink.frames.size(), 1);
    QCOMPARE(sink.frames.constFirst().identity.sourceDecodedSequence, qint64(11));
    worker.m_scheduledCutFrame.store(1, std::memory_order_release);
    worker.m_scheduledCutTargetMs.store(2000, std::memory_order_release);
    worker.m_armSeekGen.store(2, std::memory_order_release);
    worker.m_stagingCovers.store(true, std::memory_order_release);
    worker.m_cutArmed.store(true, std::memory_order_release);
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }
    const uint64_t generationBeforeLoss = GpuGenerationCounter::instance().current();
    const uint64_t lossCountBefore = GpuDeviceLossMonitor::instance().lossCount();
    worker.m_gpuRhi->injectDeviceLostForTest();
    QVERIFY(worker.m_gpuRhi->deviceLost());
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());
    QCOMPARE(GpuGenerationCounter::instance().current(), generationBeforeLoss);

    worker.m_outputRuntime->dispatchImmediate();

    const uint64_t recoveryGeneration = GpuGenerationCounter::instance().current();
    QCOMPARE(recoveryGeneration, generationBeforeLoss + 1);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), recoveryGeneration);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }
    QCOMPARE(sink.frames.size(), 2);
    QVERIFY(sink.frames.constLast().identity.videoPlaceholder);
    QVERIFY(!sink.frames.constLast().video.isGpuBacked());
    QVERIFY(sink.frames.constLast().identity.sourceDecodedSequence != qint64(11));
    QCOMPARE(worker.cutsFired(), 0);
    QVERIFY(!worker.m_cutArmed.load(std::memory_order_acquire));
    QCOMPARE(worker.m_scheduledCutFrame.load(std::memory_order_acquire), qint64(-1));

    worker.makeOutputSnapshot();
    QCOMPARE(GpuGenerationCounter::instance().current(), recoveryGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }

    worker.handleGpuDeviceLoss();
    QCOMPARE(GpuGenerationCounter::instance().current(), recoveryGeneration);
    QCOMPARE(GpuDeviceLossMonitor::instance().lossCount(), lossCountBefore + 1);
    QCOMPARE(worker.cutsFired(), 0);
    QVERIFY(!worker.m_cutArmed.load(std::memory_order_acquire));
    worker.m_outputRuntime->setEndpoints({});
}

void TestGpuDeviceLostWorker::pendingSeekDeviceLossPublishesPlaceholderSafeRecovery() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_outputRuntime->stopRuntime();
    QVERIFY(installTestGpuSpine(worker));
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(gpuFrame(worker.m_gpuRhi, 0, lostGeneration, 31));
        worker.m_outputCache->insertVideoFrame(placeholderFrame(100));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.handleGpuDeviceLoss();
    const uint64_t recoveryGeneration = GpuGenerationCounter::instance().current();

    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), recoveryGeneration);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    QVERIFY(!published->videoFrameAtFreshForGeneration(0, 0, recoveryGeneration).has_value());
    for (const FrameHandle& frame : published->videoFramesSnapshot())
        QVERIFY(!frame.isGpuBacked());
}

void TestGpuDeviceLostWorker::pendingSeekMemoryPressurePublishesPlaceholderSafeRecovery() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    worker.m_outputRuntime->stopRuntime();
    QVERIFY(installTestGpuSpine(worker));
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(2, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(0, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(0, std::memory_order_release);
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(gpuFrame(worker.m_gpuRhi, 0, lostGeneration, 41));
        worker.m_outputCache->insertVideoFrame(placeholderFrame(100));
        worker.publishOutputCacheLocked();
    }
    int resetCountBefore = 0;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        resetCountBefore = worker.m_outputRuntime->playEpochResetCountForTest();
    }

    worker.evaluateGpuMemoryPressureForTest(64 * 1024 * 1024, false, 1000);
    const uint64_t recoveryGeneration = GpuGenerationCounter::instance().current();

    QCOMPARE(worker.m_committedGpuGeneration.load(std::memory_order_acquire), recoveryGeneration);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        QCOMPARE(worker.m_outputRuntime->playEpochResetCountForTest(), resetCountBefore + 1);
    }
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    QVERIFY(!published->videoFrameAtFreshForGeneration(0, 0, recoveryGeneration).has_value());
    for (const FrameHandle& frame : published->videoFramesSnapshot())
        QVERIFY(!frame.isGpuBacked());
}

void TestGpuDeviceLostWorker::suspendedRecoveryDispatchesCommittedCpuIdentityOnce() {
    qputenv("OLR_GPU_PIPELINE", "1");
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    DefaultIosGpuLifecycleSink lifecycle;
    setIosGpuLifecycleSink(&lifecycle);

    FrameProvider feedProvider;
    QObject previewConsumer;
    feedProvider.addDirectPreviewConsumer(&previewConsumer);
    QSignalSpy frameSpy(&feedProvider, &FrameProvider::frameChanged);
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    transport.seek(100);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.initializeOutputGraph(1, 64, 48);
    QVERIFY(installTestGpuSpine(worker));
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(1, std::memory_order_release);
    worker.m_committedGeneration.store(1, std::memory_order_release);
    worker.m_committedPlayheadMs.store(100, std::memory_order_release);
    worker.m_lastVisiblePlayheadMs.store(100, std::memory_order_release);
    const uint64_t lostGeneration = GpuGenerationCounter::instance().current();
    worker.m_committedGpuGeneration.store(lostGeneration, std::memory_order_release);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 100;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.gpuGeneration = lostGeneration;
    meta.decodedSequence = 51;
    const auto data = std::make_shared<CachedGpuFrameData>(yuvPlanes(64, 48, 76, 92, 108));
    {
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache->insertVideoFrame(FrameHandle(data, meta));
        worker.publishOutputCacheLocked();
    }
    worker.setFeedPreviewProvidersEnabled(true);
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime->resetFrameIndex(1000000);
    }
    lifecycle.onEnterBackground();

    worker.handleGpuDeviceLoss();

    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(worker.m_committedPlayheadMs.load(std::memory_order_acquire), qint64(100));
    const std::shared_ptr<const OutputFrameCache> published = worker.m_publishedCache.load();
    QVERIFY(published);
    const std::optional<FrameHandle> recovered = published->videoFrameAt(0, 100);
    QVERIFY(recovered.has_value());
    QVERIFY(!recovered->isGpuBacked());
    QCOMPARE(recovered->metadata().decodedSequence, qint64(51));
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        const OutputEndpoint* preview =
            findEndpoint(worker.m_outputRuntime->outputEndpointsForTest(),
                         OutputTargetKind::QtPreview, OutputBusId::feed(0));
        QVERIFY(preview);
        QVERIFY(!asAsyncSink(preview));
    }

    lifecycle.onEnterForeground();
    worker.resumeDeferredGpuRebuild();
    worker.m_outputRuntime->stopRuntime();
    QCOMPARE(frameSpy.count(), 1);
}

QTEST_GUILESS_MAIN(TestGpuDeviceLostWorker)
#include "tst_gpu_devicelost_worker.moc"
