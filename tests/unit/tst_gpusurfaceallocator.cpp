// The surface allocator is the spec Section 10 OOM-safe degrade chokepoint: a
// GPU mint that the budget denies, or that alloc-fails, returns a CPU-backed
// handle from the fallback and bumps the OOM telemetry.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfaceallocator.h"
#include "playback/output/framehandle.h"

class TestGpuSurfaceAllocator : public QObject {
    Q_OBJECT
private slots:
    void budgetDenyDegradesToCpuNeverNull();
    void invalidRhiDegradesToCpuNeverNull();
    void invalidSurfaceCountsOomDegrade();
    void invalidFallbackStillDegradesToCpuPlaceholder();
    void budgetDenyClearsGpuGenerationOnCpuDegrade();
    void injectedAllocFailureCountsOomDegrade();
    void zeroByteSurfaceDegradesToCpu();
    void headroomMintsChargedGpuHandle();
    void customFactoryReceivesBudgetCharge();
    void customTagPropagatesToBudgetCharge();
    void degradedInvalidReadbackDoesNotSubmit();
    void degradedPreSubmitExceptionDoesNotSubmit();
    void degradedPostSubmitExceptionRetainsAndRecordsFailure();
};

namespace {

class TestSurface final : public GpuSurface {
public:
    explicit TestSurface(GpuSurfaceCompatibility compatibility = {})
        : m_compatibility(compatibility) {}

    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }
    void* nativeHandle() const override { return nullptr; }
    void retainUntilFenceRetired(uint64_t fenceValue) override { m_pendingFence = fenceValue; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    GpuSurfaceCompatibility m_compatibility;
    uint64_t m_pendingFence = 0;
};

class TestFence final : public GpuFence {
public:
    TestFence(uintptr_t deviceDomainId = 0, uint64_t authorityEpoch = 0)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override {
        ++m_signalCalls;
        return ++m_value;
    }
    bool wait(uint64_t value, int) override {
        m_lastWait = value;
        return m_completed >= value;
    }
    uint64_t completedValue() const override { return m_completed; }
    int signalCalls() const { return m_signalCalls; }
    uint64_t lastSignaledValue() const { return m_value; }
    uint64_t lastWaitValue() const { return m_lastWait; }
    void complete(uint64_t value) { m_completed = value; }

private:
    uint64_t m_value = 0;
    uint64_t m_completed = 0;
    uint64_t m_lastWait = 0;
    int m_signalCalls = 0;
};

class ZeroByteSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 0, 0}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
};

CpuPlanes solidPlanes(int w, int h) {
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = w;
    p.height = h;
    p.stride[0] = w;
    p.stride[1] = (w + 1) / 2;
    p.stride[2] = (w + 1) / 2;
    p.plane[0] = QByteArray(w * h, char(16));
    p.plane[1] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), char(128));
    p.plane[2] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), char(128));
    return p;
}

std::shared_ptr<GpuRhiContext> testRhi() {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) rhi = GpuRhiContext::createWarpForTest();
    return rhi;
}

void configureDeniedGpuBudget() {
    GpuBudgetConfig config;
    config.aggregateDecodeWindow = 0;
    config.feedCount = 1;
    config.stagingWindowPerFeed = 0;
    config.activeBusCount = 0;
    config.readbackRingDepth = 0;
    config.width = 64;
    config.height = 48;
    GpuBudget::instance().reset();
    GpuBudget::instance().configure(config);
}

FrameMetadata testFrameMetadata() {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();
    return meta;
}

} // namespace

void TestGpuSurfaceAllocator::budgetDenyDegradesToCpuNeverNull() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 0;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    auto surface = std::make_shared<TestSurface>();
    bool factoryCalled = false;
    GpuMintResult r = mintGpuOrDegrade(
        surface, meta,
        [&factoryCalled](std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge) {
            factoryCalled = true;
            return FrameHandle{};
        },
        [] { return solidPlanes(64, 48); });
    QVERIFY(!factoryCalled);
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isPresentable());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.degradedToCpu);
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::invalidRhiDegradesToCpuNeverNull() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    auto surface = std::make_shared<TestSurface>();
    auto rhi = GpuRhiContext::createInvalidForTest();
    QVERIFY(rhi != nullptr);
    QVERIFY(!rhi->isValid());
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuMintResult r =
        mintGpuOrDegrade(surface, rhi, meta, nullptr, [] { return solidPlanes(64, 48); });
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isPresentable());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.degradedToCpu);
    QCOMPARE(b.liveBytes(), qint64(0));
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::invalidSurfaceCountsOomDegrade() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    bool factoryCalled = false;
    GpuMintResult r = mintGpuOrDegrade(
        nullptr, meta,
        [&factoryCalled](std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge) {
            factoryCalled = true;
            return FrameHandle{};
        },
        [] { return solidPlanes(64, 48); });

    QVERIFY(!factoryCalled);
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isPresentable());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.degradedToCpu);
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::invalidFallbackStillDegradesToCpuPlaceholder() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 0;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuMintResult r = mintGpuOrDegrade(nullptr, nullptr, meta, nullptr, [] { return CpuPlanes{}; });
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isPresentable());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.handle.metadata().key.isPlaceholder);
    QVERIFY(r.degradedToCpu);
    QCOMPARE(r.handle.metadata().gpuGeneration, uint64_t(0));
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::budgetDenyClearsGpuGenerationOnCpuDegrade() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 0;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    auto surface = std::make_shared<TestSurface>();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.gpuGeneration = 7;

    bool factoryCalled = false;
    GpuMintResult r = mintGpuOrDegrade(
        surface, meta,
        [&factoryCalled](std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge) {
            factoryCalled = true;
            return FrameHandle{};
        },
        [] { return solidPlanes(64, 48); });

    QVERIFY(!factoryCalled);
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isPresentable());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.degradedToCpu);
    QCOMPARE(r.handle.metadata().gpuGeneration, uint64_t(0));
    QVERIFY(!r.handle.isStaleForGeneration(8));
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::injectedAllocFailureCountsOomDegrade() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    gpuSetInjectedAllocFailures(1);

    auto surface = std::make_shared<TestSurface>();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    bool factoryCalled = false;
    GpuMintResult r = mintGpuOrDegrade(
        surface, meta,
        [&factoryCalled](std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge) {
            factoryCalled = true;
            return FrameHandle{};
        },
        [] { return solidPlanes(64, 48); });

    gpuSetInjectedAllocFailures(0);
    QVERIFY(!factoryCalled);
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isPresentable());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.degradedToCpu);
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::zeroByteSurfaceDegradesToCpu() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    auto surface = std::make_shared<ZeroByteSurface>();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    bool factoryCalled = false;
    GpuMintResult r = mintGpuOrDegrade(
        surface, meta,
        [&factoryCalled](std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge) {
            factoryCalled = true;
            return FrameHandle{};
        },
        [] { return solidPlanes(64, 48); });

    QVERIFY(!factoryCalled);
    QVERIFY(!r.handle.isNull());
    QVERIFY(!r.handle.isGpuBacked());
    QVERIFY(r.degradedToCpu);
    QCOMPARE(b.liveBytes(), qint64(0));
    QCOMPARE(b.oomDegradeCount(), qint64(1));
}

void TestGpuSurfaceAllocator::headroomMintsChargedGpuHandle() {
    auto rhi = testRhi();
    if (!rhi) QSKIP("no test RHI backend on this host");

    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    constexpr uintptr_t deviceDomain = 0xA110C;
    constexpr uint64_t authorityEpoch = 1;
    auto surface =
        std::make_shared<TestSurface>(GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
    auto renderFence = std::make_shared<TestFence>(deviceDomain, authorityEpoch);
    GpuRetireRegistry retireRegistry;
    const qsizetype pendingBefore = retireRegistry.diagnostics().pendingRetains;
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuMintResult r =
        mintGpuOrDegrade(surface, rhi, meta, renderFence, [] { return solidPlanes(64, 48); });
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isGpuBacked());
    QVERIFY(!r.degradedToCpu);
    QVERIFY(renderFence->lastSignaledValue() > uint64_t(0));
    const auto* frameData = dynamic_cast<const GpuFrameData*>(r.handle.data());
    QVERIFY(frameData != nullptr);
    QVERIFY(!frameData->waitForPendingFence(0));
    QCOMPARE(renderFence->lastWaitValue(), renderFence->lastSignaledValue());
    QCOMPARE(retireRegistry.diagnostics().pendingRetains, pendingBefore + 1);
    QVERIFY(b.liveBytes() > 0);
    renderFence->complete(renderFence->lastSignaledValue());
    QVERIFY(frameData->waitForPendingFence(0));
    retireRegistry.drainCompleted();
    QCOMPARE(retireRegistry.diagnostics().pendingRetains, pendingBefore);
}

void TestGpuSurfaceAllocator::customFactoryReceivesBudgetCharge() {
    auto rhi = testRhi();
    if (!rhi) QSKIP("no test RHI backend on this host");

    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    auto surface = std::make_shared<TestSurface>();
    auto renderFence = std::make_shared<TestFence>();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuMintResult r = mintGpuOrDegrade(
        surface, meta,
        [rhi, renderFence](std::shared_ptr<GpuSurface> s, FrameMetadata m,
                           GpuBudgetCharge charge) mutable {
            return makeGpuFrameHandle(std::move(s), std::move(rhi), std::move(m),
                                      std::move(renderFence), std::move(charge));
        },
        [] { return solidPlanes(64, 48); });

    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isGpuBacked());
    QVERIFY(!r.degradedToCpu);
    QVERIFY(b.liveBytes() > 0);
}

void TestGpuSurfaceAllocator::customTagPropagatesToBudgetCharge() {
    auto rhi = testRhi();
    if (!rhi) QSKIP("no test RHI backend on this host");

    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 8;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    auto surface = std::make_shared<TestSurface>();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuBudgetTag observedTag = GpuBudgetTag::Other;
    GpuMintResult r = mintGpuOrDegrade(
        surface, meta,
        [rhi, &observedTag](std::shared_ptr<GpuSurface> s, FrameMetadata m,
                            GpuBudgetCharge charge) mutable {
            observedTag = charge.tag();
            return makeGpuFrameHandle(std::move(s), std::move(rhi), std::move(m), nullptr,
                                      std::move(charge));
        },
        [] { return solidPlanes(64, 48); }, GpuBudgetTag::ReadbackRing);

    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isGpuBacked());
    QCOMPARE(observedTag, GpuBudgetTag::ReadbackRing);
    QCOMPARE(b.liveBytes(GpuBudgetTag::ReadbackRing), gpuSurfaceBytes(*surface));
    QCOMPARE(b.gatedLiveBytes(), gpuSurfaceBytes(*surface));
}

void TestGpuSurfaceAllocator::degradedInvalidReadbackDoesNotSubmit() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    configureDeniedGpuBudget();

    const GpuSurfaceCompatibility compatibility{0xA110C, monitor.currentDeviceAuthorityForTest()};
    auto surface = std::make_shared<TestSurface>(compatibility);
    std::weak_ptr<GpuSurface> weakSurface = surface;
    auto contextFence =
        std::make_shared<TestFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto callerFence =
        std::make_shared<TestFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto rhi = GpuRhiContext::createReadbackForTest(
        GpuReadbackTestBehavior::InvalidBeforeSubmission, contextFence);
    QVERIFY(rhi != nullptr);
    QVERIFY(rhi->isValid());
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t lossCountBefore = monitor.lossCount();

    const GpuMintResult result =
        mintGpuOrDegrade(surface, rhi, testFrameMetadata(), callerFence, [surface, rhi] {
            return submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p).planes;
        });
    surface.reset();

    QVERIFY(result.degradedToCpu);
    QCOMPARE(contextFence->signalCalls(), 0);
    QCOMPARE(callerFence->signalCalls(), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(weakSurface.expired());
    QVERIFY(!monitor.isLost());
    QCOMPARE(monitor.lossCount(), lossCountBefore);
}

void TestGpuSurfaceAllocator::degradedPreSubmitExceptionDoesNotSubmit() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    configureDeniedGpuBudget();

    const GpuSurfaceCompatibility compatibility{0xA110C, monitor.currentDeviceAuthorityForTest()};
    auto surface = std::make_shared<TestSurface>(compatibility);
    std::weak_ptr<GpuSurface> weakSurface = surface;
    auto contextFence =
        std::make_shared<TestFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto callerFence =
        std::make_shared<TestFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto rhi = GpuRhiContext::createReadbackForTest(GpuReadbackTestBehavior::ThrowBeforeSubmission,
                                                    contextFence);
    QVERIFY(rhi != nullptr);
    QVERIFY(rhi->isValid());
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t lossCountBefore = monitor.lossCount();

    const GpuMintResult result =
        mintGpuOrDegrade(surface, rhi, testFrameMetadata(), callerFence, [surface, rhi] {
            return submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p).planes;
        });
    surface.reset();

    QVERIFY(result.degradedToCpu);
    QCOMPARE(contextFence->signalCalls(), 0);
    QCOMPARE(callerFence->signalCalls(), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(weakSurface.expired());
    QVERIFY(!monitor.isLost());
    QCOMPARE(monitor.lossCount(), lossCountBefore);
}

void TestGpuSurfaceAllocator::degradedPostSubmitExceptionRetainsAndRecordsFailure() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    configureDeniedGpuBudget();

    const GpuSurfaceCompatibility compatibility{0xA110C, monitor.currentDeviceAuthorityForTest()};
    auto surface = std::make_shared<TestSurface>(compatibility);
    std::weak_ptr<GpuSurface> weakSurface = surface;
    auto contextFence =
        std::make_shared<TestFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto callerFence =
        std::make_shared<TestFence>(compatibility.deviceDomainId, compatibility.authorityEpoch);
    auto rhi = GpuRhiContext::createReadbackForTest(GpuReadbackTestBehavior::ThrowAfterSubmission,
                                                    contextFence);
    QVERIFY(rhi != nullptr);
    QVERIFY(rhi->isValid());
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t lossCountBefore = monitor.lossCount();

    const GpuMintResult result =
        mintGpuOrDegrade(surface, rhi, testFrameMetadata(), callerFence, [surface, rhi] {
            return submitGpuReadback(rhi, surface, FramePixelFormat::Yuv420p).planes;
        });
    surface.reset();

    QVERIFY(result.degradedToCpu);
    QCOMPARE(contextFence->signalCalls(), 1);
    QCOMPARE(callerFence->signalCalls(), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(!weakSurface.expired());
    QVERIFY(monitor.isLost());
    QCOMPARE(monitor.lossCount(), lossCountBefore + 1);

    contextFence->complete(1);
    registry.drainCompleted();
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QVERIFY(weakSurface.expired());
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

QTEST_GUILESS_MAIN(TestGpuSurfaceAllocator)
#include "tst_gpusurfaceallocator.moc"
