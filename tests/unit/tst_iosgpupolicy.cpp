// The iOS GPU budget is deliberately more aggressive than macOS (thermal + VRAM).
// This test pins the documented caps and proves the iOS clamp is strictly below
// the macOS window-derived cap for the same track count.
#include <QtTest>

#include "playback/gpu/iosgpupolicy.h"

class TestIosGpuPolicy : public QObject {
    Q_OBJECT
private slots:
    void hostIsNotIosBuild();
    void hostCapKeepsMacFormula();
    void iosCapsAreDocumentedAndTight();
    void perTrackCapNeverExceedsIosCeilingOnIos();
};

void TestIosGpuPolicy::hostIsNotIosBuild() {
    QVERIFY(!gpuIsIosBuild());
}

void TestIosGpuPolicy::hostCapKeepsMacFormula() {
    if (gpuIsIosBuild()) QSKIP("macOS cap branch only active on the host target");
    QCOMPARE(gpuPerTrackWindowCap(0), 256);
    QCOMPARE(gpuPerTrackWindowCap(1), 256);
    QCOMPARE(gpuPerTrackWindowCap(16), 16);
    QCOMPARE(gpuPerTrackWindowCap(64), 12);
}

void TestIosGpuPolicy::iosCapsAreDocumentedAndTight() {
    QVERIFY(kIosMaxPerTrackGpuFrames > 0);
    QVERIFY(kIosMaxPerTrackGpuFrames < 12);
    QVERIFY(kIosAggregateGpuFrameCeiling < 256);
    QCOMPARE(gpuIosAggregateWindowCeiling(), kIosAggregateGpuFrameCeiling);
}

void TestIosGpuPolicy::perTrackCapNeverExceedsIosCeilingOnIos() {
    if (!gpuIsIosBuild()) QSKIP("iOS clamp branch only active on the iOS target");
    for (int n = 1; n <= 8; ++n) {
        QVERIFY(gpuPerTrackWindowCap(n) <= kIosMaxPerTrackGpuFrames);
    }
}

QTEST_GUILESS_MAIN(TestIosGpuPolicy)
#include "tst_iosgpupolicy.moc"
