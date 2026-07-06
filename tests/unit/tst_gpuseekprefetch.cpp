#include <QtTest>

#include "playback/gpu/gpuseekprefetch.h"

class TestGpuSeekPrefetch : public QObject {
    Q_OBJECT
private slots:
    void forwardPlanCoversLeadWindow();
    void reversePlanCoversLeadWindow();
    void prefetchBoundedByHeadroom();
    void noHeadroomNoPrefetch();
};

void TestGpuSeekPrefetch::forwardPlanCoversLeadWindow() {
    const GpuPrefetchPlan p =
        GpuSeekPrefetch::planPrefetch(1000, 1, 16, 500, 3110400, qint64(3110400) * 256, 0);

    QCOMPARE(p.startMs, int64_t(1000));
    QCOMPARE(p.endMs, int64_t(1500));
    QVERIFY(p.surfaceCount >= 31 && p.surfaceCount <= 32);
}

void TestGpuSeekPrefetch::reversePlanCoversLeadWindow() {
    const GpuPrefetchPlan p =
        GpuSeekPrefetch::planPrefetch(1000, -1, 40, 800, 3110400, qint64(3110400) * 64, 0);

    QCOMPARE(p.startMs, int64_t(200));
    QCOMPARE(p.endMs, int64_t(1000));
    QCOMPARE(p.surfaceCount, 20);
}

void TestGpuSeekPrefetch::prefetchBoundedByHeadroom() {
    const GpuPrefetchPlan p = GpuSeekPrefetch::planPrefetch(
        1000, 1, 16, 500, 3110400, qint64(3110400) * 100, qint64(3110400) * 96);

    QCOMPARE(p.surfaceCount, 4);
}

void TestGpuSeekPrefetch::noHeadroomNoPrefetch() {
    const GpuPrefetchPlan p = GpuSeekPrefetch::planPrefetch(
        1000, 1, 16, 500, 3110400, qint64(3110400) * 100, qint64(3110400) * 100);

    QCOMPARE(p.surfaceCount, 0);
    QCOMPARE(p.startMs, int64_t(-1));
    QCOMPARE(p.endMs, int64_t(-1));
}

QTEST_GUILESS_MAIN(TestGpuSeekPrefetch)
#include "tst_gpuseekprefetch.moc"
