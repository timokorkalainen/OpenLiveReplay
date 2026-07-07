#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "recorder_engine/codec/gpuencodepump.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

class FakeEncoder final : public NativeVideoEncoder {
public:
    std::atomic<int> calls{0};
    std::atomic<int> cpuCalls{0};
    std::atomic<bool> failSurfaceEncode{false};
    std::atomic<bool> cpuEncodeSucceeds{false};
    std::mutex* expectedEncoderMutex = nullptr;
    std::atomic<bool> sawEncoderMutexHeld{false};
    std::atomic<bool> blockSurfaceEncode{false};
    std::atomic<int> blockedSurfaceEncodeEntries{0};
    std::mutex blockMutex;
    std::condition_variable blockCv;
    bool releaseBlockedSurfaceEncode = false;

    bool encode(const AVFrame*, int64_t ptsTicks, const PacketCallback& onPacket,
                QString*) override {
        cpuCalls.fetch_add(1, std::memory_order_acq_rel);
        if (!cpuEncodeSucceeds.load(std::memory_order_acquire)) return false;
        onPacket(QByteArray("cpu-pkt"), ptsTicks, true);
        return true;
    }

    bool encodeSurface(GpuSurface*, int64_t ptsTicks, const ColorMetadata&,
                       const PacketCallback& onPacket, QString*) override {
        calls.fetch_add(1, std::memory_order_acq_rel);
        if (expectedEncoderMutex) {
            std::atomic<bool> probeLocked{false};
            std::thread probe([&] {
                if (expectedEncoderMutex->try_lock()) {
                    probeLocked.store(true, std::memory_order_release);
                    expectedEncoderMutex->unlock();
                }
            });
            probe.join();
            sawEncoderMutexHeld.store(!probeLocked.load(std::memory_order_acquire),
                                      std::memory_order_release);
        }
        if (blockSurfaceEncode.load(std::memory_order_acquire)) {
            blockedSurfaceEncodeEntries.fetch_add(1, std::memory_order_acq_rel);
            std::unique_lock<std::mutex> lock(blockMutex);
            blockCv.wait(lock, [&] { return releaseBlockedSurfaceEncode; });
        }
        if (failSurfaceEncode.load(std::memory_order_acquire)) return false;
        onPacket(QByteArray("pkt"), ptsTicks, true);
        return true;
    }

    bool flush(const PacketCallback&, QString*) override { return true; }
    QByteArray avccExtradata() const override { return QByteArray("avcc"); }

    void releaseBlockedEncodes() {
        {
            std::lock_guard<std::mutex> lock(blockMutex);
            releaseBlockedSurfaceEncode = true;
        }
        blockCv.notify_all();
    }
};

class FakeSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return const_cast<FakeSurface*>(this); }
};

class FakeGpuFrameData final : public IFrameData {
public:
    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override {
        return solidYuv420pHandle(16, 16, 70, 128, 128).readToCpu(target);
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<FakeSurface> m_surface = std::make_shared<FakeSurface>();
};

class UnreadableGpuFrameData final : public IFrameData {
public:
    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override {
        m_readCount.fetch_add(1, std::memory_order_acq_rel);
        return CpuPlanes{};
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }
    int readCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    std::shared_ptr<FakeSurface> m_surface = std::make_shared<FakeSurface>();
    mutable std::atomic<int> m_readCount{0};
};

class FakeFence final : public GpuFence {
public:
    uint64_t signal() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_completedValue;
        m_cv.notify_all();
        return m_completedValue;
    }

    bool wait(uint64_t value, int timeoutMs) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (timeoutMs < 0) {
            m_cv.wait(lock, [&] { return m_completedValue >= value; });
            return true;
        }
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [&] { return m_completedValue >= value; });
    }

    uint64_t completedValue() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_completedValue;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_completedValue = 0;
};

FrameHandle makeGpuHandle() {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;
    return FrameHandle(std::make_shared<FakeGpuFrameData>(), meta);
}

FrameHandle makeUnreadableGpuHandle(const std::shared_ptr<UnreadableGpuFrameData>& data =
                                        std::make_shared<UnreadableGpuFrameData>()) {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;
    return FrameHandle(data, meta);
}

} // namespace

class TestGpuEncodePump : public QObject {
    Q_OBJECT
private slots:
    void encodeWaitsForFenceThenForwardsPacket();
    void encodeSurfaceFailureReportsDroppedJob();
    void encodeSurfaceFailureDropsJobWithoutCpuFallback();
    void unreadableGpuHandleDropsWithoutCpuReadback();
    void packetCallbackRunsAfterEncoderMutexReleased();
    void submitBackpressuresOnOverflowUntilQueueSpace();
    void cancelPendingDropsQueuedJobsWithoutInvokingCallbacks();
};

void TestGpuEncodePump::encodeWaitsForFenceThenForwardsPacket() {
    FakeEncoder enc;
    auto fence = std::make_shared<FakeFence>();

    GpuEncodePump pump(&enc, fence, 4);
    pump.start();

    std::atomic<int> packets{0};
    QVERIFY(pump.submit(makeGpuHandle(), 1, 100, ColorMetadata{},
                        [&](const QByteArray&, int64_t, bool) {
                            packets.fetch_add(1, std::memory_order_acq_rel);
                        }));

    QTest::qWait(60);
    QCOMPARE(enc.calls.load(std::memory_order_acquire), 0);

    fence->signal();
    QTRY_COMPARE_WITH_TIMEOUT(enc.calls.load(std::memory_order_acquire), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(packets.load(std::memory_order_acquire), 1, 2000);

    pump.stop();
    QCOMPARE(pump.framesEncoded(), uint64_t(1));
}

void TestGpuEncodePump::encodeSurfaceFailureReportsDroppedJob() {
    FakeEncoder enc;
    enc.failSurfaceEncode.store(true, std::memory_order_release);
    auto fence = std::make_shared<FakeFence>();
    fence->signal();

    GpuEncodePump pump(&enc, fence, 4);
    pump.start();

    std::atomic<int> failures{0};
    QVERIFY(pump.submit(
        makeGpuHandle(), 1, 100, ColorMetadata{}, [](const QByteArray&, int64_t, bool) {},
        [&] { failures.fetch_add(1, std::memory_order_acq_rel); }));

    QTRY_COMPARE_WITH_TIMEOUT(enc.calls.load(std::memory_order_acquire), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(failures.load(std::memory_order_acquire), 1, 2000);

    pump.stop();
    QCOMPARE(enc.cpuCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(pump.framesEncoded(), uint64_t(0));
    QCOMPARE(pump.queueDrops(), uint64_t(1));
}

void TestGpuEncodePump::encodeSurfaceFailureDropsJobWithoutCpuFallback() {
    FakeEncoder enc;
    enc.failSurfaceEncode.store(true, std::memory_order_release);
    enc.cpuEncodeSucceeds.store(true, std::memory_order_release);
    auto fence = std::make_shared<FakeFence>();
    fence->signal();
    const auto data = std::make_shared<UnreadableGpuFrameData>();

    GpuEncodePump pump(&enc, fence, 4);
    pump.start();

    std::atomic<int> packets{0};
    std::atomic<int> failures{0};
    QVERIFY(pump.submit(
        makeGpuHandle(), 1, 100, ColorMetadata{},
        [&](const QByteArray&, int64_t, bool) { packets.fetch_add(1, std::memory_order_acq_rel); },
        [&] { failures.fetch_add(1, std::memory_order_acq_rel); }));

    QTRY_COMPARE_WITH_TIMEOUT(enc.calls.load(std::memory_order_acquire), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(failures.load(std::memory_order_acquire), 1, 2000);

    pump.stop();
    QCOMPARE(enc.cpuCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(packets.load(std::memory_order_acquire), 0);
    QCOMPARE(pump.framesEncoded(), uint64_t(0));
    QCOMPARE(pump.queueDrops(), uint64_t(1));
}

void TestGpuEncodePump::unreadableGpuHandleDropsWithoutCpuReadback() {
    FakeEncoder enc;
    enc.failSurfaceEncode.store(true, std::memory_order_release);
    enc.cpuEncodeSucceeds.store(true, std::memory_order_release);
    auto fence = std::make_shared<FakeFence>();
    fence->signal();
    const auto data = std::make_shared<UnreadableGpuFrameData>();

    GpuEncodePump pump(&enc, fence, 4);
    pump.start();

    std::atomic<int> packets{0};
    std::atomic<int> failures{0};
    QVERIFY(pump.submit(
        makeUnreadableGpuHandle(data), 1, 100, ColorMetadata{},
        [&](const QByteArray&, int64_t, bool) { packets.fetch_add(1, std::memory_order_acq_rel); },
        [&] { failures.fetch_add(1, std::memory_order_acq_rel); }));

    QTRY_COMPARE_WITH_TIMEOUT(enc.calls.load(std::memory_order_acquire), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(failures.load(std::memory_order_acquire), 1, 2000);

    pump.stop();
    QCOMPARE(data->readCount(), 0);
    QCOMPARE(enc.cpuCalls.load(std::memory_order_acquire), 0);
    QCOMPARE(packets.load(std::memory_order_acquire), 0);
    QCOMPARE(pump.framesEncoded(), uint64_t(0));
    QCOMPARE(pump.queueDrops(), uint64_t(1));
}

void TestGpuEncodePump::packetCallbackRunsAfterEncoderMutexReleased() {
    FakeEncoder enc;
    auto fence = std::make_shared<FakeFence>();
    fence->signal();
    std::mutex encoderMutex;
    enc.expectedEncoderMutex = &encoderMutex;

    GpuEncodePump pump(&enc, fence, 4, &encoderMutex);
    pump.start();

    std::atomic<int> packets{0};
    std::atomic<bool> callbackCouldLockEncoderMutex{false};
    QVERIFY(pump.submit(makeGpuHandle(), 1, 100, ColorMetadata{},
                        [&](const QByteArray&, int64_t, bool) {
                            std::atomic<bool> locked{false};
                            std::thread probe([&] {
                                if (encoderMutex.try_lock()) {
                                    locked.store(true, std::memory_order_release);
                                    encoderMutex.unlock();
                                }
                            });
                            probe.join();
                            callbackCouldLockEncoderMutex.store(
                                locked.load(std::memory_order_acquire), std::memory_order_release);
                            packets.fetch_add(1, std::memory_order_acq_rel);
                        }));

    QTRY_COMPARE_WITH_TIMEOUT(packets.load(std::memory_order_acquire), 1, 2000);
    QVERIFY(enc.sawEncoderMutexHeld.load(std::memory_order_acquire));
    QVERIFY(callbackCouldLockEncoderMutex.load(std::memory_order_acquire));

    pump.stop();
}

void TestGpuEncodePump::submitBackpressuresOnOverflowUntilQueueSpace() {
    FakeEncoder enc;
    enc.blockSurfaceEncode.store(true, std::memory_order_release);
    auto fence = std::make_shared<FakeFence>();
    fence->signal();

    GpuEncodePump pump(&enc, fence, 1);
    pump.start();

    std::atomic<int> packets{0};
    std::atomic<int> failures{0};
    auto onPacket = [&](const QByteArray&, int64_t, bool) {
        packets.fetch_add(1, std::memory_order_acq_rel);
    };
    auto onFailure = [&] { failures.fetch_add(1, std::memory_order_acq_rel); };

    QVERIFY(pump.submit(makeGpuHandle(), 1, 1, ColorMetadata{}, onPacket, onFailure));
    QTRY_COMPARE_WITH_TIMEOUT(enc.blockedSurfaceEncodeEntries.load(std::memory_order_acquire), 1,
                              2000);
    QVERIFY(pump.submit(makeGpuHandle(), 1, 2, ColorMetadata{}, onPacket, onFailure));

    std::atomic<bool> returned{false};
    bool thirdSubmit = false;
    std::thread submitter([&] {
        thirdSubmit = pump.submit(makeGpuHandle(), 1, 3, ColorMetadata{}, onPacket, onFailure);
        returned.store(true, std::memory_order_release);
    });

    QTest::qWait(100);
    const bool returnedEarly = returned.load(std::memory_order_acquire);
    enc.releaseBlockedEncodes();
    for (int i = 0; i < 200 && !returned.load(std::memory_order_acquire); ++i)
        QTest::qWait(10);

    const bool returnedAfterRelease = returned.load(std::memory_order_acquire);
    if (!returnedAfterRelease) pump.stop();
    if (submitter.joinable()) submitter.join();
    pump.stop();

    QVERIFY2(!returnedEarly, "submit must backpressure instead of dropping queued recorder frames");
    QVERIFY2(returnedAfterRelease, "submit must resume once the encode worker opens queue space");
    QVERIFY(thirdSubmit);
    QTRY_COMPARE_WITH_TIMEOUT(enc.calls.load(std::memory_order_acquire), 3, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(packets.load(std::memory_order_acquire), 3, 2000);

    QCOMPARE(pump.queueDrops(), uint64_t(0));
    QCOMPARE(failures.load(std::memory_order_acquire), 0);
}

void TestGpuEncodePump::cancelPendingDropsQueuedJobsWithoutInvokingCallbacks() {
    FakeEncoder enc;
    auto fence = std::make_shared<FakeFence>();
    GpuEncodePump pump(&enc, fence, 4);

    std::atomic<int> packets{0};
    std::atomic<int> failures{0};
    auto onPacket = [&](const QByteArray&, int64_t, bool) {
        packets.fetch_add(1, std::memory_order_acq_rel);
    };
    auto onFailure = [&] { failures.fetch_add(1, std::memory_order_acq_rel); };

    QVERIFY(pump.submit(makeGpuHandle(), 9, 1, ColorMetadata{}, onPacket, onFailure));
    QVERIFY(pump.submit(makeGpuHandle(), 9, 2, ColorMetadata{}, onPacket, onFailure));

    pump.cancelPending();

    QCOMPARE(pump.queueDrops(), uint64_t(2));
    QCOMPARE(packets.load(std::memory_order_acquire), 0);
    QCOMPARE(failures.load(std::memory_order_acquire), 0);
}

QTEST_GUILESS_MAIN(TestGpuEncodePump)
#include "tst_gpuencodepump.moc"
