// The surface allocator is the spec Section 10 OOM-safe degrade chokepoint: a
// GPU mint that the budget denies, or that alloc-fails, returns a CPU-backed
// handle from the fallback and bumps the OOM telemetry.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpupipelineconfig.h"
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
};

namespace {

class TestSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
    void retainUntilFenceRetired(uint64_t fenceValue) override { m_pendingFence = fenceValue; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    uint64_t m_pendingFence = 0;
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

    auto surface = std::make_shared<TestSurface>();
    auto renderFence = GpuFence::create();
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    GpuMintResult r =
        mintGpuOrDegrade(surface, rhi, meta, renderFence, [] { return solidPlanes(64, 48); });
    QVERIFY(!r.handle.isNull());
    QVERIFY(r.handle.isGpuBacked());
    QVERIFY(!r.degradedToCpu);
    QVERIFY(surface->pendingFenceValue() > uint64_t(0));
    QVERIFY(b.liveBytes() > 0);
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
    auto renderFence = GpuFence::create();
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

QTEST_GUILESS_MAIN(TestGpuSurfaceAllocator)
#include "tst_gpusurfaceallocator.moc"
