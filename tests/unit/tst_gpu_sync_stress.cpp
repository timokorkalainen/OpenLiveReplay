#include <QtTest>

#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/output/framehandle.h"
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
    void stubFenceStressIsRaceFree();
};

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
    GpuGenerationCounter::instance().reset();
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
