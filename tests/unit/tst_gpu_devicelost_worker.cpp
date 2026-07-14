#include <QtTest>

#include "playback/frameprovider.h"
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
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
#include <atomic>
#include <thread>
#include <utility>

#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch, uintptr_t deviceDomainId) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
            deviceDomainId);
    }
    static void setProofDeliveryGate(QSemaphore* accepted, QSemaphore* proceed) {
        auto& monitor = GpuDeviceLossMonitor::instance();
        monitor.m_proofAcceptedForTest = accepted;
        monitor.m_continueProofDeliveryForTest = proceed;
    }
};
#endif

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
    void boundedLossWaitDoesNotHoldRecoveryEpochLock();
    void lateDeadDomainProofReleasesQuarantineAutomatically();
    void tokenlessUpgradeDuringRecoveryReleasesQuarantineAutomatically();
    void acceptedProofDeliveryCompletesBeforeRebuildCanClearState();

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

class BlockingLossFence final : public GpuFence {
public:
    BlockingLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t value, int) override {
        waitEntered.release();
        releaseWait.acquire();
        m_completed.store(value, std::memory_order_release);
        return true;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }

    QSemaphore waitEntered;
    QSemaphore releaseWait;

private:
    uint64_t m_signalled = 0;
    std::atomic<uint64_t> m_completed{0};
};

class IncompleteLossFence final : public GpuFence {
public:
    IncompleteLossFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t, int) override { return false; }
    uint64_t completedValue() const override { return 0; }

private:
    uint64_t m_signalled = 0;
};

class CompatibleLossSurface final : public GpuSurface {
public:
    CompatibleLossSurface(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : m_compatibility{deviceDomainId, authorityEpoch} {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override { return reinterpret_cast<void*>(0xB10C); }

private:
    GpuSurfaceCompatibility m_compatibility;
};

struct SubmittedAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

class CachedGpuFrameData final : public IFrameData {
public:
    explicit CachedGpuFrameData(CpuPlanes planes) : m_planes(std::move(planes)) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override {
        m_readToCpuCalls++;
        return CpuPlanes{};
    }
    CpuPlanes cachedCpuPlanes(FramePixelFormat target) const override {
        return target == m_planes.format ? m_planes : CpuPlanes{};
    }
    GpuSurface* gpuSurface() const override { return nullptr; }
    FramePixelFormat nativeFormat() const override { return m_planes.format; }
    int readToCpuCalls() const { return m_readToCpuCalls; }

private:
    CpuPlanes m_planes;
    mutable int m_readToCpuCalls = 0;
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

void TestGpuDeviceLostWorker::boundedLossWaitDoesNotHoldRecoveryEpochLock() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t deviceDomain = 0xB10C;
    const uint64_t authority = monitor.currentDeviceAuthorityForTest();

    GpuRetireRegistry registry;
    auto fence = std::make_shared<BlockingLossFence>(deviceDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(
        operation
            .submit(adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
            .retirement,
        GpuRetirementDisposition::Published);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);

    std::thread recovery([&]() { worker.handleGpuDeviceLoss(); });
    const bool waitStarted = fence->waitEntered.tryAcquire(1, 5000);
    std::atomic<bool> rebuildReturned{false};
    std::thread rebuild;
    if (waitStarted) {
        rebuild = std::thread([&]() {
            monitor.beginRebuild();
            rebuildReturned.store(true, std::memory_order_release);
        });
    }

    QElapsedTimer deadline;
    deadline.start();
    while (waitStarted && !rebuildReturned.load(std::memory_order_acquire) &&
           deadline.elapsed() < 1000)
        QThread::msleep(1);
    const bool rebuildReturnedBeforeFenceRelease = rebuildReturned.load(std::memory_order_acquire);
    fence->releaseWait.release();
    if (rebuild.joinable()) rebuild.join();
    recovery.join();

    QVERIFY(waitStarted);
    QVERIFY2(rebuildReturnedBeforeFenceRelease,
             "bounded fence wait must run after the recovery epoch mutex is released");
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
}

void TestGpuDeviceLostWorker::lateDeadDomainProofReleasesQuarantineAutomatically() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t firstDomain = 0xA110;
    constexpr uintptr_t lateDomain = 0xB220;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto firstFence = std::make_shared<IncompleteLossFence>(firstDomain, authority);
    auto lateFence = std::make_shared<IncompleteLossFence>(lateDomain, authority);
    auto firstSurface = std::make_shared<CompatibleLossSurface>(firstDomain, authority);
    auto lateSurface = std::make_shared<CompatibleLossSurface>(lateDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope firstOperation(firstFence, registry);
    GpuOpScope lateOperation(lateFence, registry);
    QCOMPARE(firstOperation
                 .submit(adapter, GpuSurfacePack<1>(
                                      std::array<std::shared_ptr<GpuSurface>, 1>{firstSurface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(lateOperation
                 .submit(adapter,
                         GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{lateSurface}))
                 .retirement,
             GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, firstDomain) != 0);
    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    worker.handleGpuDeviceLoss();
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, lateDomain) != 0);

    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::tokenlessUpgradeDuringRecoveryReleasesQuarantineAutomatically() {
    qunsetenv("OLR_GPU_PIPELINE");
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t deviceDomain = 0xC330;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();

    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(deviceDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(deviceDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(
        operation
            .submit(adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
            .retirement,
        GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    FrameProvider feedProvider;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    PlaybackWorker worker({&feedProvider}, &transport);
    worker.m_gpuPipelineState.store(static_cast<int>(PlaybackWorker::GpuPipelineState::Gpu),
                                    std::memory_order_release);
    QSemaphore beforeTokenless;
    QSemaphore continueTokenless;
    worker.m_gpuBeforeTokenlessRecoveryEnteredForTest = &beforeTokenless;
    worker.m_gpuContinueTokenlessRecoveryForTest = &continueTokenless;

    std::thread recovery([&]() { worker.handleGpuDeviceLoss(); });
    const bool reachedUpgradeWindow = beforeTokenless.tryAcquire(1, 5000);
    const bool proofPublished = reachedUpgradeWindow && GpuDeviceLossMonitorTestAuthority::publish(
                                                            authority, deviceDomain) != 0;
    continueTokenless.release();
    recovery.join();
    worker.m_gpuBeforeTokenlessRecoveryEnteredForTest = nullptr;
    worker.m_gpuContinueTokenlessRecoveryForTest = nullptr;

    QVERIFY(reachedUpgradeWindow);
    QVERIFY(proofPublished);
    QCOMPARE(worker.gpuPipelineState(), PlaybackWorker::GpuPipelineState::CpuFallback);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuDeviceLostWorker::acceptedProofDeliveryCompletesBeforeRebuildCanClearState() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t firstDomain = 0xD440;
    constexpr uintptr_t lateDomain = 0xE550;
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<IncompleteLossFence>(lateDomain, authority);
    auto surface = std::make_shared<CompatibleLossSurface>(lateDomain, authority);
    SubmittedAdapter adapter;
    GpuOpScope operation(fence, registry);
    QCOMPARE(
        operation
            .submit(adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
            .retirement,
        GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, firstDomain) != 0);
    QCOMPARE(
        monitor
            .withValidatedDeadDomains([](const GpuValidatedDeadDomains&) { return qsizetype(0); })
            .status,
        GpuValidatedLossStatus::Completed);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QSemaphore proofAccepted;
    QSemaphore continueDelivery;
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(&proofAccepted, &continueDelivery);
    std::thread publisher(
        [&]() { (void) GpuDeviceLossMonitorTestAuthority::publish(authority, lateDomain); });
    const bool accepted = proofAccepted.tryAcquire(1, 5000);
    std::atomic<bool> rebuildCleared{false};
    QSemaphore rebuildAttempting;
    std::thread rebuild;
    if (accepted) {
        rebuild = std::thread([&]() {
            rebuildAttempting.release();
            monitor.beginRebuild();
            monitor.clearForRebuild();
            rebuildCleared.store(true, std::memory_order_release);
        });
    }
    const bool rebuildReachedBegin = accepted && rebuildAttempting.tryAcquire(1, 5000);

    QElapsedTimer deadline;
    deadline.start();
    while (rebuildReachedBegin && !rebuildCleared.load(std::memory_order_acquire) &&
           deadline.elapsed() < 250)
        QThread::msleep(1);
    const bool clearedBeforeDelivery = rebuildCleared.load(std::memory_order_acquire);
    continueDelivery.release();
    publisher.join();
    if (rebuild.joinable()) rebuild.join();
    GpuDeviceLossMonitorTestAuthority::setProofDeliveryGate(nullptr, nullptr);

    QVERIFY(accepted);
    QVERIFY(rebuildReachedBegin);
    QVERIFY2(!clearedBeforeDelivery,
             "accepted proof must be delivered before rebuild clears state");
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
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

QTEST_GUILESS_MAIN(TestGpuDeviceLostWorker)
#include "tst_gpu_devicelost_worker.moc"
