// GpuFence is the backend-matched render/readback ordering primitive. The
// interface is uniform; concrete backends use MTLSharedEvent on Apple,
// ID3D11Fence on Windows, and a deterministic timeline stub elsewhere.
#include <QtTest>

#include "playback/gpu/gpufence.h"

class TestGpuFence : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void signalWaitRoundTrips();
    void waitTimesOutBeforeSignal();
};

void TestGpuFence::createIsNullOrValidNeverPartial() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");
    QCOMPARE(fence->completedValue(), uint64_t(0));
}

void TestGpuFence::signalWaitRoundTrips() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");
    const uint64_t value = fence->signal();
    QVERIFY(value >= 1);
    QVERIFY(fence->wait(value, 1000));
    QVERIFY(fence->completedValue() >= value);
}

void TestGpuFence::waitTimesOutBeforeSignal() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");
    QVERIFY(!fence->wait(fence->completedValue() + 100, 50));
}

QTEST_GUILESS_MAIN(TestGpuFence)
#include "tst_gpufence.moc"
