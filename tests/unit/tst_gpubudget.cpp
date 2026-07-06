// GpuBudget sizes the VRAM cap from the spec Section 2 peak formula. The decode
// window is aggregate (<=256, not multiplied by feeds); the armed-cut staging
// bank is per-feed. This pins the multiview multiplier so the budget is never a
// flat 32-64 surfaces.
#include <QtTest>

#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpusurface.h"

#include <utility>

namespace {

class AllocSizeSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override {
        GpuSurfaceDesc d{FramePixelFormat::Nv12, 64, 48};
        d.allocationBytes = 8192;
        return d;
    }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
};

} // namespace

class TestGpuBudget : public QObject {
    Q_OBJECT
private slots:
    void surfaceBytesIsNv12();
    void outputSurfaceBytesDefaultsToRgba8();
    void surfaceBytesPrefersReportedAllocationBytes();
    void peakFormulaCountsStagingPerFeedNotDecodeWindow();
    void peakFormulaBudgetsRgbaBusOutputs();
    void canAllocateGatesAgainstBudget();
    void chargeCreditRoundTrips();
    void chargeTokenCreditsOnDestruction();
    void chargeTokenMoveTransfersOwnership();
    void tryChargeDeniesWhenBudgetFull();
};

void TestGpuBudget::surfaceBytesIsNv12() {
    GpuBudgetConfig c;
    c.width = 1920;
    c.height = 1080;
    c.surfaceFormat = FramePixelFormat::Nv12;
    QCOMPARE(c.surfaceBytes(), qint64(1920) * 1080 * 3 / 2);
}

void TestGpuBudget::outputSurfaceBytesDefaultsToRgba8() {
    GpuBudgetConfig c;
    c.width = 1920;
    c.height = 1080;
    QCOMPARE(c.outputSurfaceBytes(), qint64(1920) * 1080 * 4);
}

void TestGpuBudget::surfaceBytesPrefersReportedAllocationBytes() {
    AllocSizeSurface surface;

    QCOMPARE(gpuSurfaceBytes(surface), qint64(8192));
}

void TestGpuBudget::peakFormulaCountsStagingPerFeedNotDecodeWindow() {
    GpuBudgetConfig c;
    c.feedCount = 8;
    c.aggregateDecodeWindow = 256;
    c.stagingWindowPerFeed = 32;
    c.activeBusCount = 3;
    c.readbackRingDepth = 3;
    c.width = 1920;
    c.height = 1080;
    const qint64 sb = c.surfaceBytes();
    const qint64 rgba = c.outputSurfaceBytes();

    QCOMPARE(c.peakBudgetBytes(), qint64(521) * sb + qint64(3) * rgba);
    QVERIFY(c.peakBudgetBytes() > qint64(64) * sb);
}

void TestGpuBudget::peakFormulaBudgetsRgbaBusOutputs() {
    GpuBudgetConfig c;
    c.feedCount = 1;
    c.aggregateDecodeWindow = 0;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 2;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    c.surfaceFormat = FramePixelFormat::Nv12;

    QCOMPARE(c.peakBudgetBytes(), qint64(2) * c.outputSurfaceBytes());
}

void TestGpuBudget::canAllocateGatesAgainstBudget() {
    GpuBudgetConfig c;
    c.feedCount = 1;
    c.aggregateDecodeWindow = 4;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;

    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    QCOMPARE(b.budgetBytes(), qint64(4) * sb);
    QVERIFY(b.canAllocate(4 * sb));
    QVERIFY(!b.canAllocate(5 * sb));
}

void TestGpuBudget::chargeCreditRoundTrips() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 2;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;

    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    QCOMPARE(b.liveBytes(), qint64(0));
    b.charge(sb);
    QCOMPARE(b.liveBytes(), sb);
    QVERIFY(!b.canAllocate(2 * sb));
    b.credit(sb);
    QCOMPARE(b.liveBytes(), qint64(0));
}

void TestGpuBudget::chargeTokenCreditsOnDestruction() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 4;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;

    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    {
        GpuBudgetCharge charge(sb);
        QCOMPARE(b.liveBytes(), sb);
    }
    QCOMPARE(b.liveBytes(), qint64(0));
}

void TestGpuBudget::chargeTokenMoveTransfersOwnership() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 4;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;

    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    GpuBudgetCharge charge(sb);
    QCOMPARE(b.liveBytes(), sb);
    {
        GpuBudgetCharge moved = std::move(charge);
        QCOMPARE(b.liveBytes(), sb);
    }
    QCOMPARE(b.liveBytes(), qint64(0));
}

void TestGpuBudget::tryChargeDeniesWhenBudgetFull() {
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 1;
    c.feedCount = 1;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;

    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);
    const qint64 sb = c.surfaceBytes();
    auto first = b.tryCharge(sb);
    QVERIFY(first.has_value());
    QCOMPARE(b.liveBytes(), sb);
    auto second = b.tryCharge(sb);
    QVERIFY(!second.has_value());
    QCOMPARE(b.liveBytes(), sb);
}

QTEST_GUILESS_MAIN(TestGpuBudget)
#include "tst_gpubudget.moc"
