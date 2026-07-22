// GpuFrameData is the GPU concrete of IFrameData: it reports GPU residency,
// exposes its surface, and downloads lazily through GpuRhiContext.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpuframeretirequeue.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusubmission.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "playback/output/gpureadbacktelemetry.h"
#include "playback/output/outputframecache.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/vtkeepsurfaceimporter.h"
#endif

class TestGpuFrameData : public QObject {
    Q_OBJECT
private slots:
    void gpuBackedReportsSurface();
    void completedReadbackRetainReleasesImmediately();
    void waitForPendingFenceUsesProducerFence();
    void exactFenceWaitIgnoresOtherTimelineWatermark();
    void exactFenceWaitHandlesZeroAndRejectsMissingFence();
    void frameDataExposesOneExactProducerSynchronizationPair();
    void invalidReadbackBeforeSubmissionDoesNotRetireOrLatchLoss();
    void exceptionBeforeSubmissionDoesNotRetireOrLatchLoss();
    void exceptionAfterSubmissionRetainsAndLatchesFailure();
    void readbackConsumesPackedSurfaceNotDecoy();
    void droppingGpuHandleCreditsBudget();
#ifdef __APPLE__
    void gpuPresentabilityDoesNotReadBack();
    void outputCacheInsertionDoesNotReadBack();
    void readToCpuDownloadsAndCounts();
    void cpuReadbackCacheChargesRepeatedReadsOnce();
    void cpuReadbackCacheCreditsWhenHandleIsDestroyed();
    void appleSurfaceWithoutRhiReadsToCpu();
    void readbackMatchesCpuWithinOneLsb();
    void importVtBufferProducesGpuHandle();
    void readbackStampsSurfacePendingFence();
    void readbackRetainsSurfaceWhenEvictionSawNoPendingFence();
#endif
};

namespace {

class DeferredFence final : public GpuFence {
public:
    DeferredFence() : GpuFence(0xD3F3, 1) {}
    DeferredFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override {
        signalCalls.fetch_add(1, std::memory_order_acq_rel);
        return m_next.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    bool wait(uint64_t value, int) override {
        waitCalls.fetch_add(1, std::memory_order_acq_rel);
        lastWaitValue.store(value, std::memory_order_release);
        return completedValue() >= value;
    }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

    std::atomic<int> waitCalls{0};
    std::atomic<int> signalCalls{0};
    std::atomic<uint64_t> lastWaitValue{0};

private:
    std::atomic<uint64_t> m_next{0};
    std::atomic<uint64_t> m_completed{0};
};

class TestSurface final : public GpuSurface {
public:
    explicit TestSurface(GpuSurfaceCompatibility compatibility = {0xD3F3, 1},
                         void* nativeHandle = reinterpret_cast<void*>(0xD3F3),
                         uint32_t subresource = 0)
        : m_compatibility(compatibility), m_nativeHandle(nativeHandle), m_subresource(subresource) {
    }
    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }
    void* nativeHandle() const override { return m_nativeHandle; }
    uint32_t nativeSubresource() const override { return m_subresource; }

private:
    GpuSurfaceCompatibility m_compatibility;
    void* m_nativeHandle = nullptr;
    uint32_t m_subresource = 0;
};

#ifdef __APPLE__
qint64 cpuPlanePayloadBytes(const CpuPlanes& planes) {
    qint64 bytes = 0;
    for (const QByteArray& plane : planes.plane) {
        bytes += qint64(plane.size());
    }
    return bytes;
}
#endif

} // namespace

void TestGpuFrameData::gpuBackedReportsSurface() {
#ifdef __APPLE__
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    QVERIFY(handle.isGpuBacked());
    QVERIFY(handle.data()->gpuSurface() != nullptr);
    QCOMPARE(handle.data()->nativeFormat(), FramePixelFormat::Nv12);
#else
    QSKIP("GPU backend is Apple-only in Phase 2");
#endif
}

void TestGpuFrameData::completedReadbackRetainReleasesImmediately() {
    GpuRetireRegistry{}.drainCompleted();

    auto fence = std::make_shared<DeferredFence>();
    fence->complete(1);
    auto surface = std::make_shared<TestSurface>();
    std::weak_ptr<GpuSurface> weakSurface = surface;

    GpuRetireRegistry registry;
    GpuOpScope operation(fence, registry);
    auto adapter = [](const GpuScopedNativeView<1>&) noexcept {
        return GpuSubmitOutcome::Submitted;
    };
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    registry.drainCompleted();
    surface.reset();

    QVERIFY2(weakSurface.expired(),
             "already-completed readback fences must not leave a surface retained until a later "
             "opportunistic drain");
    QCOMPARE(GpuRetireRegistry{}.pendingRetainCount(), qsizetype(0));
}

void TestGpuFrameData::waitForPendingFenceUsesProducerFence() {
    auto fence = std::make_shared<DeferredFence>();
    auto surface = std::make_shared<TestSurface>();
    surface->retainUntilFenceRetired(1);

    GpuFrameData data(surface, nullptr, FramePixelFormat::Nv12, {}, fence, {}, 0, 1);
    QVERIFY(!data.waitForPendingFence(0));
    fence->complete(1);
    QVERIFY(data.waitForPendingFence(0));
    QCOMPARE(fence->lastWaitValue.load(std::memory_order_acquire), uint64_t(1));
}

void TestGpuFrameData::exactFenceWaitIgnoresOtherTimelineWatermark() {
    auto producerFence = std::make_shared<DeferredFence>();
    auto otherFence = std::make_shared<DeferredFence>();
    auto surface = std::make_shared<TestSurface>();
    const uint64_t producerFenceValue = producerFence->signal();
    surface->retainUntilFenceRetired(producerFenceValue);
    for (int i = 0; i < 4; ++i) {
        surface->retainUntilFenceRetired(otherFence->signal());
    }
    QCOMPARE(surface->pendingFenceValue(), uint64_t(4));

    GpuFrameData data(surface, nullptr, FramePixelFormat::Nv12, {}, producerFence, {}, 0,
                      producerFenceValue);
    producerFence->complete(producerFenceValue);
    QVERIFY(data.waitForPendingFence(0));
    QCOMPARE(producerFence->lastWaitValue.load(std::memory_order_acquire), producerFenceValue);
}

void TestGpuFrameData::exactFenceWaitHandlesZeroAndRejectsMissingFence() {
    auto fence = std::make_shared<DeferredFence>();
    auto surface = std::make_shared<TestSurface>();
    surface->retainUntilFenceRetired(9);

    GpuFrameData noSubmission(surface, nullptr, FramePixelFormat::Nv12, {}, fence, {}, 0, 0);
    QVERIFY(noSubmission.waitForPendingFence(0));
    QCOMPARE(fence->waitCalls.load(std::memory_order_acquire), 0);

    GpuFrameData mismatchedPair(surface, nullptr, FramePixelFormat::Nv12, {}, nullptr, {}, 0, 1);
    QVERIFY(!mismatchedPair.waitForPendingFence(0));
}

void TestGpuFrameData::frameDataExposesOneExactProducerSynchronizationPair() {
    auto firstFence = std::make_shared<DeferredFence>();
    auto secondFence = std::make_shared<DeferredFence>();
    auto surface = std::make_shared<TestSurface>();
    const uint64_t firstValue = firstFence->signal();
    for (int i = 0; i < 4; ++i)
        (void) secondFence->signal();
    surface->retainUntilFenceRetired(4);

    GpuFrameData data(surface, nullptr, FramePixelFormat::Nv12, {}, firstFence, {}, 0, firstValue);
    const GpuFrameSynchronization synchronization = data.gpuSynchronization();
    QCOMPARE(synchronization.fence, firstFence);
    QCOMPARE(synchronization.value, firstValue);
    QVERIFY(synchronization.isExact());
    QVERIFY(synchronization.value != surface->pendingFenceValue());
}

void TestGpuFrameData::invalidReadbackBeforeSubmissionDoesNotRetireOrLatchLoss() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const GpuSurfaceCompatibility compatibility{0xD3F3, monitor.currentDeviceAuthorityForTest()};
    auto producerFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto contextFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto surface = std::make_shared<TestSurface>(compatibility);
    auto rhi = GpuRhiContext::createReadbackForTest(
        GpuReadbackTestBehavior::InvalidBeforeSubmission, contextFence);
    QVERIFY(rhi != nullptr);
    GpuFrameData data(surface, rhi, FramePixelFormat::Nv12, {}, producerFence);

    const CpuPlanes planes = data.readToCpu(FramePixelFormat::Yuv420p);

    QVERIFY(!planes.isValid());
    QCOMPARE(contextFence->signalCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(producerFence->signalCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(0));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(!monitor.isLost());
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuFrameData::exceptionBeforeSubmissionDoesNotRetireOrLatchLoss() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const GpuSurfaceCompatibility compatibility{0xD3F3, monitor.currentDeviceAuthorityForTest()};
    auto producerFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto contextFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto surface = std::make_shared<TestSurface>(compatibility);
    auto rhi = GpuRhiContext::createReadbackForTest(GpuReadbackTestBehavior::ThrowBeforeSubmission,
                                                    contextFence);
    QVERIFY(rhi != nullptr);
    GpuFrameData data(surface, rhi, FramePixelFormat::Nv12, {}, producerFence);

    const CpuPlanes planes = data.readToCpu(FramePixelFormat::Yuv420p);

    QVERIFY(!planes.isValid());
    QCOMPARE(contextFence->signalCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(producerFence->signalCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(!monitor.isLost());
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuFrameData::exceptionAfterSubmissionRetainsAndLatchesFailure() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const GpuSurfaceCompatibility compatibility{0xD3F3, monitor.currentDeviceAuthorityForTest()};
    auto producerFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto contextFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto surface = std::make_shared<TestSurface>(compatibility);
    auto rhi = GpuRhiContext::createReadbackForTest(GpuReadbackTestBehavior::ThrowAfterSubmission,
                                                    contextFence);
    QVERIFY(rhi != nullptr);
    GpuFrameData data(surface, rhi, FramePixelFormat::Nv12, {}, producerFence);

    const CpuPlanes planes = data.readToCpu(FramePixelFormat::Yuv420p);

    QVERIFY(!planes.isValid());
    QCOMPARE(contextFence->signalCalls.load(std::memory_order_acquire), 1);
    QCOMPARE(producerFence->signalCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(monitor.isLost());

    contextFence->complete(1);
    registry.drainCompleted();
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuFrameData::readbackConsumesPackedSurfaceNotDecoy() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const GpuSurfaceCompatibility compatibility{0xD3F4, monitor.currentDeviceAuthorityForTest()};
    auto fence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto packed =
        std::make_shared<TestSurface>(compatibility, reinterpret_cast<void*>(0xA11CE), 11);
    auto decoy = std::make_shared<TestSurface>(compatibility, reinterpret_cast<void*>(0xDEC0), 22);
    auto rhi = GpuRhiContext::createReadbackForTest(
        GpuReadbackTestBehavior::InvalidBeforeSubmission, fence);
    QVERIFY(rhi != nullptr);

    (void) decoy;
    const GpuReadbackResult result = submitGpuReadback(rhi, packed, FramePixelFormat::Yuv420p);

    QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    QVERIFY(GpuRhiContextTestAuthority::lastReadbackHadNativeHandleForTest(rhi));
    QCOMPARE(GpuRhiContextTestAuthority::lastReadbackSubresourceForTest(rhi), uint32_t(11));
    QVERIFY(GpuRhiContextTestAuthority::lastReadbackSubresourceForTest(rhi) != uint32_t(22));
    QCOMPARE(fence->signalCalls.load(std::memory_order_acquire), 0);
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuFrameData::droppingGpuHandleCreditsBudget() {
    auto fence = std::make_shared<DeferredFence>();
    auto surface = std::make_shared<TestSurface>();
    GpuBudget::instance().reset();
    const qint64 bytes = gpuSurfaceBytes(*surface);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    {
        FrameHandle handle = makeGpuFrameHandle(surface, std::shared_ptr<GpuRhiContext>{}, meta,
                                                fence, GpuBudgetCharge(bytes));
        QCOMPARE(GpuBudget::instance().liveBytes(), bytes);
    }
    QCOMPARE(GpuBudget::instance().liveBytes(), qint64(0));
}

#ifdef __APPLE__
void TestGpuFrameData::gpuPresentabilityDoesNotReadBack() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 40;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);
    QVERIFY(handle.isPresentable());
    QCOMPARE(data->readToCpuCount(), 0);
}

void TestGpuFrameData::outputCacheInsertionDoesNotReadBack() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 40;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);

    OutputFrameCache cache(1, 64, 48);
    cache.insertVideoFrame(handle);
    QCOMPARE(data->readToCpuCount(), 0);

    auto cached = cache.videoFrameAt(0, 40);
    QVERIFY(cached.has_value());
    QVERIFY(cached->isGpuBacked());
}

void TestGpuFrameData::readToCpuDownloadsAndCounts() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    GpuReadbackTelemetry::instance().reset();
    gpuResetFrameReadToCpuCount();

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);
    QCOMPARE(data->readToCpuCount(), 0);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(0));
    QVERIFY(!data->cachedCpuPlanes(FramePixelFormat::Yuv420p).isValid());
    const CpuPlanes planes = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(planes.isValid());
    QCOMPARE(planes.width, 64);
    QCOMPARE(planes.height, 48);
    QCOMPARE(data->readToCpuCount(), 1);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(1));
    QVERIFY(data->cachedCpuPlanes(FramePixelFormat::Yuv420p).isValid());

    const GpuReadbackTelemetrySnapshot once = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(once.gpuReadbacks, qint64(0));
    QCOMPARE(once.uniqueSurfaces, qint64(0));
    QCOMPARE(once.redundantReadbacks, qint64(0));

    const CpuPlanes second = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(second.isValid());
    QCOMPARE(data->readToCpuCount(), 1);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(1));

    const GpuReadbackTelemetrySnapshot twice = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(twice.gpuReadbacks, qint64(0));
    QCOMPARE(twice.uniqueSurfaces, qint64(0));
    QCOMPARE(twice.redundantReadbacks, qint64(0));
}

void TestGpuFrameData::cpuReadbackCacheChargesRepeatedReadsOnce() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto& budget = GpuBudget::instance();
    budget.reset();

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);

    const CpuPlanes planes = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(planes.isValid());
    const qint64 cachedBytes = cpuPlanePayloadBytes(planes);
    QVERIFY(cachedBytes > 0);
    QCOMPARE(budget.liveBytes(GpuBudgetTag::CpuReadbackCache), cachedBytes);
    QCOMPARE(budget.gatedLiveBytes(), qint64(0));
    QCOMPARE(data->readToCpuCount(), 1);

    const CpuPlanes second = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(second.isValid());
    QCOMPARE(data->readToCpuCount(), 1);
    QCOMPARE(budget.liveBytes(GpuBudgetTag::CpuReadbackCache), cachedBytes);
    QCOMPARE(budget.gatedLiveBytes(), qint64(0));

    handle = FrameHandle();
    QCOMPARE(budget.liveBytes(GpuBudgetTag::CpuReadbackCache), qint64(0));
}

void TestGpuFrameData::cpuReadbackCacheCreditsWhenHandleIsDestroyed() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto& budget = GpuBudget::instance();
    budget.reset();

    qint64 cachedBytes = 0;
    {
        auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
        QVERIFY(surface != nullptr);
        FrameMetadata meta;
        meta.key.format = FramePixelFormat::Nv12;
        meta.key.width = 64;
        meta.key.height = 48;

        FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
        const CpuPlanes planes = handle.readToCpu(FramePixelFormat::Yuv420p);
        QVERIFY(planes.isValid());
        cachedBytes = cpuPlanePayloadBytes(planes);
        QVERIFY(cachedBytes > 0);
        QCOMPARE(budget.liveBytes(GpuBudgetTag::CpuReadbackCache), cachedBytes);
    }
    QCOMPARE(budget.liveBytes(GpuBudgetTag::CpuReadbackCache), qint64(0));
    QCOMPARE(budget.liveBytes(), qint64(0));
}

void TestGpuFrameData::appleSurfaceWithoutRhiReadsToCpu() {
    gpuResetFrameReadToCpuCount();

    auto surface = makeAppleNv12Surface(16, 16);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;

    FrameHandle handle = makeGpuFrameHandle(surface, nullptr, meta);
    const CpuPlanes planes = handle.readToCpu(FramePixelFormat::Yuv420p);

    QVERIFY(planes.isValid());
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 16);
    QCOMPARE(planes.height, 16);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(1));
}

void TestGpuFrameData::readbackMatchesCpuWithinOneLsb() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(16, 16, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const CpuPlanes got = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(got.isValid());
    for (char b : got.plane[0])
        QVERIFY(qAbs(static_cast<int>(static_cast<uchar>(b))) <= 1);
    for (char b : got.plane[1])
        QVERIFY(qAbs(static_cast<int>(static_cast<uchar>(b))) <= 1);
    for (char b : got.plane[2])
        QVERIFY(qAbs(static_cast<int>(static_cast<uchar>(b))) <= 1);
}

void TestGpuFrameData::importVtBufferProducesGpuHandle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.ptsMs = 40;

    FrameHandle handle = importVtSurface(surface, meta, rhi);
    QVERIFY(handle.isGpuBacked());
    QCOMPARE(handle.metadata().key.format, FramePixelFormat::Nv12);
    QCOMPARE(handle.metadata().key.width, 64);
    QCOMPARE(handle.metadata().key.height, 48);
}

void TestGpuFrameData::readbackStampsSurfacePendingFence() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto renderFence = rhi->createFence();
    QVERIFY(renderFence != nullptr);

    auto surface = makeAppleNv12Surface(64, 48, rhi->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta, renderFence);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(0));
    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    QVERIFY(surface->pendingFenceValue() >= uint64_t(1));
}

void TestGpuFrameData::readbackRetainsSurfaceWhenEvictionSawNoPendingFence() {
    const uint64_t authority = GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch();
    auto contextFence = std::make_shared<DeferredFence>(uintptr_t(0xA11), authority);
    auto rhi = GpuRhiContext::createWithReadbackFenceForTest(contextFence);
    if (!rhi) QSKIP("no RHI backend");
    const GpuSurfaceCompatibility compatibility = rhi->surfaceCompatibility();
    auto surface = makeAppleNv12Surface(64, 48, compatibility);
    QVERIFY(surface != nullptr);
    auto producerFence =
        std::make_shared<DeferredFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    std::weak_ptr<GpuSurface> weakSurface = surface;

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta, producerFence);
    surface.reset();

    GpuFrameRetireQueue preReadbackRetireQueue;
    preReadbackRetireQueue.collect(handle);
    QCOMPARE(preReadbackRetireQueue.size(), 0);

    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    QVERIFY(!weakSurface.expired());
    QCOMPARE(contextFence->signalCalls.load(std::memory_order_acquire), 1);
    QCOMPARE(producerFence->signalCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(weakSurface.lock()->pendingFenceValue(), uint64_t(1));
    handle = FrameHandle();
    QVERIFY(!weakSurface.expired());
    QVERIFY(GpuRetireRegistry{}.pendingRetainCount() >= 1);

    contextFence->complete(1);
    GpuRetireRegistry{}.drainCompleted();
    QVERIFY(weakSurface.expired());
}
#endif

QTEST_GUILESS_MAIN(TestGpuFrameData)
#include "tst_gpuframedata.moc"
