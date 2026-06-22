// DecodeDoneFence lets a producer publish decode completion and a consumer wait
// before touching a GPU surface. The full GPU sync model builds on this.
#include <QtTest>

#include "playback/gpu/decodedonefence.h"

#include <chrono>
#include <thread>

class TestDecodeDoneFence : public QObject {
    Q_OBJECT
private slots:
    void signalThenWaitSucceeds();
    void waitBlocksUntilSignaledFromAnotherThread();
    void signaledValueAdvances();
    void waitForValueReachesGpuTimeline();
};

void TestDecodeDoneFence::signalThenWaitSucceeds() {
    auto fence = DecodeDoneFence::create();
    if (!fence) QSKIP("no fence backend on this platform");
    QVERIFY(!fence->isSignaled());
    fence->signalDecodeDone();
    QVERIFY(fence->waitDecodeDone(1000));
    QVERIFY(fence->isSignaled());
}

void TestDecodeDoneFence::waitBlocksUntilSignaledFromAnotherThread() {
    auto fence = DecodeDoneFence::create();
    if (!fence) QSKIP("no fence backend on this platform");

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fence->signalDecodeDone();
    });
    QVERIFY(fence->waitDecodeDone(2000));
    producer.join();
}

void TestDecodeDoneFence::signaledValueAdvances() {
    auto fence = DecodeDoneFence::create();
    if (!fence) QSKIP("no fence backend on this platform");
    QCOMPARE(fence->signaledValue(), uint64_t(0));
    fence->signalDecodeDone();
    QVERIFY(fence->signaledValue() >= 1);
    const uint64_t v1 = fence->signaledValue();
    fence->signalDecodeDone();
    QVERIFY(fence->signaledValue() > v1);
}

void TestDecodeDoneFence::waitForValueReachesGpuTimeline() {
    auto fence = DecodeDoneFence::create();
    if (!fence) QSKIP("no fence backend on this platform");
    QVERIFY(!fence->waitForValue(1, 50));
    fence->signalDecodeDone();
    const uint64_t v = fence->signaledValue();
    QVERIFY(fence->waitForValue(v, 1000));
}

QTEST_GUILESS_MAIN(TestDecodeDoneFence)
#include "tst_decodedonefence.moc"
