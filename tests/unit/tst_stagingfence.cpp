#include <QtTest>

#include "playback/gpu/gpufence.h"

class TestStagingFence : public QObject {
    Q_OBJECT
private slots:
    void swapWaitsForStagingFence();
};

void TestStagingFence::swapWaitsForStagingFence() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");

    QVERIFY(!fence->wait(1, 50));
    const uint64_t staged = fence->signal();
    QVERIFY(fence->wait(staged, 1000));
}

QTEST_GUILESS_MAIN(TestStagingFence)
#include "tst_stagingfence.moc"
