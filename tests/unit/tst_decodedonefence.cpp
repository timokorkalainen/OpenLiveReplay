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

QTEST_GUILESS_MAIN(TestDecodeDoneFence)
#include "tst_decodedonefence.moc"
