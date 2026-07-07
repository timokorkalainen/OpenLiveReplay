// iOS no longer has a fixed tiny per-track ceiling; PlaybackWorker derives the
// residency cap from the active window and passes it into the policy seam.
#include <QtTest>

#include "playback/gpu/iosgpupolicy.h"

class TestIosGpuPolicy : public QObject {
    Q_OBJECT
private slots:
    void hostIsNotIosBuild();
    void hostCapKeepsMacFormula();
    void hostIgnoresDerivedCap();
    void iosUsesDerivedCapWhenPresent();
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

void TestIosGpuPolicy::hostIgnoresDerivedCap() {
    if (gpuIsIosBuild()) QSKIP("macOS cap branch only active on the host target");
    QCOMPARE(gpuPerTrackWindowCap(4, 72), 64);
}

void TestIosGpuPolicy::iosUsesDerivedCapWhenPresent() {
    if (!gpuIsIosBuild()) QSKIP("iOS clamp branch only active on the iOS target");
    QCOMPARE(gpuPerTrackWindowCap(4, 72), 72);
}

QTEST_GUILESS_MAIN(TestIosGpuPolicy)
#include "tst_iosgpupolicy.moc"
