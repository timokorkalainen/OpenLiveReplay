#include <QtTest>

#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframeretirequeue.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "playback/trackbuffer.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#endif

#include <atomic>
#include <thread>
#include <vector>

class TestGpuSyncStress : public QObject {
    Q_OBJECT
private slots:
#ifdef __APPLE__
    void concurrentEvictWhileRenderNeverFreesInUse();
    void seekUnderDecodeInvalidatesStaleSurfaces();
#endif
    void capEvictionRetireQueueHoldsGpuFramesUntilFenceCompletes();
    void stubFenceStressIsRaceFree();
};

namespace {

class StressGpuSurface final : public GpuSurface {
public:
    explicit StressGpuSurface(uint64_t pendingFence) : m_pendingFence(pendingFence) {}

    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 4, 4}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    uint64_t m_pendingFence = 0;
};

class StressGpuFrameData final : public IFrameData {
public:
    explicit StressGpuFrameData(std::shared_ptr<StressGpuSurface> surface)
        : m_surface(std::move(surface)) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override { return {}; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<StressGpuSurface> m_surface;
};

class StressFence final : public GpuFence {
public:
    uint64_t signal() override { return ++m_completedValue; }
    bool wait(uint64_t value, int) override {
        waitCalls++;
        return m_completedValue >= value;
    }
    uint64_t completedValue() const override { return m_completedValue; }

    void complete(uint64_t value) { m_completedValue = value; }

    int waitCalls = 0;

private:
    uint64_t m_completedValue = 0;
};

FrameHandle makeStressGpuFrame(int feed, qint64 ptsMs, uint64_t pendingFence,
                               std::weak_ptr<const IFrameData>* weakData = nullptr) {
    auto data = std::static_pointer_cast<const IFrameData>(
        std::make_shared<StressGpuFrameData>(std::make_shared<StressGpuSurface>(pendingFence)));
    if (weakData) *weakData = data;
    FrameMetadata meta;
    meta.key.feedIndex = feed;
    meta.key.ptsMs = ptsMs;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    return FrameHandle(std::move(data), meta);
}

} // namespace

#ifdef __APPLE__
void TestGpuSyncStress::concurrentEvictWhileRenderNeverFreesInUse() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto renderFence = GpuFence::create();
    QVERIFY(renderFence != nullptr);

    constexpr int kFeeds = 4;
    constexpr int kIters = 64;
    std::atomic<bool> sawFreedInUse{false};

    std::vector<std::thread> producers;
    producers.reserve(kFeeds);
    for (int feed = 0; feed < kFeeds; ++feed) {
        producers.emplace_back([&, feed] {
            for (int i = 0; i < kIters; ++i) {
                auto surface = makeAppleNv12Surface(64, 48);
                if (!surface) continue;
                FrameMetadata meta;
                meta.key.feedIndex = feed;
                meta.key.format = FramePixelFormat::Nv12;
                meta.key.width = 64;
                meta.key.height = 48;
                meta.gpuGeneration = GpuGenerationCounter::instance().current();
                FrameHandle cacheHandle = makeGpuFrameHandle(surface, rhi, meta);

                const uint64_t fenceValue = renderFence->signal();
                surface->retainUntilFenceRetired(fenceValue);
                surface.reset();

                FrameHandle consumer = cacheHandle;
                std::thread reader([&sawFreedInUse, consumer]() mutable {
                    if (!consumer.readToCpu(FramePixelFormat::Yuv420p).isValid())
                        sawFreedInUse.store(true, std::memory_order_release);
                });

                renderFence->wait(fenceValue, 1000);
                cacheHandle = FrameHandle();
                reader.join();
            }
        });
    }
    for (auto& thread : producers)
        thread.join();

    QVERIFY(!sawFreedInUse.load(std::memory_order_acquire));
}

void TestGpuSyncStress::seekUnderDecodeInvalidatesStaleSurfaces() {
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t initialGeneration = GpuGenerationCounter::instance().bump();
    FrameHandle handle = solidYuv420pHandle(16, 16, 16, 128, 128);
    handle.metadata().gpuGeneration = initialGeneration;
    std::atomic<bool> stalePresented{false};

    std::thread seeker([] {
        for (int i = 0; i < 100; ++i)
            GpuGenerationCounter::instance().bump();
    });
    std::thread presenter([&] {
        for (int i = 0; i < 100; ++i) {
            const uint64_t current = GpuGenerationCounter::instance().current();
            if (!handle.isStaleForGeneration(current) && current != initialGeneration)
                stalePresented.store(true, std::memory_order_release);
        }
    });
    seeker.join();
    presenter.join();

    QVERIFY(handle.isStaleForGeneration(GpuGenerationCounter::instance().current()));
    QVERIFY(!stalePresented.load(std::memory_order_acquire));
}
#endif

void TestGpuSyncStress::capEvictionRetireQueueHoldsGpuFramesUntilFenceCompletes() {
    constexpr int kFeeds = 4;
    constexpr uint64_t kFenceValue = 7;
    auto fence = std::make_shared<StressFence>();
    GpuFrameRetireQueue retireQueue;
    QVector<std::weak_ptr<const IFrameData>> evictedData;

    for (int feed = 0; feed < kFeeds; ++feed) {
        TrackBuffer buffer;
        std::weak_ptr<const IFrameData> weak;
        QVERIFY(buffer.insert(0, makeStressGpuFrame(feed, 0, kFenceValue, &weak), /*capFrames*/ 2,
                              /*keepNearMs*/ 0, /*protectToMs*/ 40));
        QVERIFY(buffer.insert(40, makeStressGpuFrame(feed, 40, kFenceValue), /*capFrames*/ 2,
                              /*keepNearMs*/ 0, /*protectToMs*/ 40));

        TrackBuffer::EvictedFrames evicted;
        QVERIFY(buffer.insert(80, makeStressGpuFrame(feed, 80, kFenceValue), /*capFrames*/ 2,
                              /*keepNearMs*/ 80, /*protectToMs*/ 120, &evicted));
        QCOMPARE(evicted.size(), 1);
        evictedData.append(weak);
        for (const TrackBuffer::Frame& frame : evicted)
            retireQueue.collect(frame.frame, fence);
    }

    int stalls = 0;
    QCOMPARE(retireQueue.drain(0, &stalls), 0);
    QCOMPARE(stalls, kFeeds);
    QCOMPARE(retireQueue.size(), kFeeds);
    QVERIFY(fence->waitCalls >= kFeeds);
    for (const auto& weak : evictedData)
        QVERIFY(!weak.expired());

    fence->complete(kFenceValue);
    stalls = 0;
    QCOMPARE(retireQueue.drain(0, &stalls), kFeeds);
    QCOMPARE(stalls, 0);
    QCOMPARE(retireQueue.size(), 0);
    for (const auto& weak : evictedData)
        QVERIFY(weak.expired());
}

void TestGpuSyncStress::stubFenceStressIsRaceFree() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");

    std::atomic<uint64_t> maxSeen{0};
    std::atomic<bool> waitFailed{false};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int threadIndex = 0; threadIndex < 4; ++threadIndex) {
        threads.emplace_back([&] {
            for (int i = 0; i < 256; ++i) {
                const uint64_t value = fence->signal();
                uint64_t previous = maxSeen.load(std::memory_order_acquire);
                while (value > previous &&
                       !maxSeen.compare_exchange_weak(previous, value, std::memory_order_acq_rel)) {
                }
                if (!fence->wait(value, 1000)) waitFailed.store(true, std::memory_order_release);
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    QVERIFY(!waitFailed.load(std::memory_order_acquire));
    QVERIFY(fence->completedValue() >= maxSeen.load(std::memory_order_acquire));
}

QTEST_GUILESS_MAIN(TestGpuSyncStress)
#include "tst_gpu_sync_stress.moc"
