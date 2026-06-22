// GpuFence is the backend-matched render/readback ordering primitive. The
// interface is uniform; concrete backends use MTLSharedEvent on Apple,
// ID3D11Fence on Windows, and a deterministic timeline stub elsewhere.
#include <QtTest>

#include "playback/gpu/gpufence.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

class TestGpuFence : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void signalWaitRoundTrips();
    void waitTimesOutBeforeSignal();
    void waitForeverReturnsAfterSignal();
    void concurrentSignalsProduceUniqueMonotonicValues();
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

void TestGpuFence::waitForeverReturnsAfterSignal() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");

    std::atomic<bool> waitReturned{false};
    std::atomic<bool> waitResult{false};
    std::thread waiter([&] {
        waitResult.store(fence->wait(1, -1), std::memory_order_release);
        waitReturned.store(true, std::memory_order_release);
    });

    QTest::qWait(20);
    QVERIFY(!waitReturned.load(std::memory_order_acquire));
    QCOMPARE(fence->signal(), uint64_t(1));
    waiter.join();
    QVERIFY(waitReturned.load(std::memory_order_acquire));
    QVERIFY(waitResult.load(std::memory_order_acquire));
}

void TestGpuFence::concurrentSignalsProduceUniqueMonotonicValues() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend on this host");

    constexpr int kThreads = 8;
    constexpr int kSignalsPerThread = 16;
    std::vector<uint64_t> values(kThreads * kSignalsPerThread, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kSignalsPerThread; ++i)
                values[size_t(t * kSignalsPerThread + i)] = fence->signal();
        });
    }
    for (std::thread& thread : threads)
        thread.join();

    std::sort(values.begin(), values.end());
    QCOMPARE(values.front(), uint64_t(1));
    for (size_t i = 0; i < values.size(); ++i)
        QCOMPARE(values[i], uint64_t(i + 1));
    QVERIFY(fence->wait(values.back(), 2000));
}

QTEST_GUILESS_MAIN(TestGpuFence)
#include "tst_gpufence.moc"
