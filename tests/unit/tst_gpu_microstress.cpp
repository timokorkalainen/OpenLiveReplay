#include <QtTest>

#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/output/framehandle.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#endif

#include <atomic>
#include <memory>
#include <thread>

class TestGpuMicrostress : public QObject {
    Q_OBJECT
private slots:
#ifdef __APPLE__
    void evictWhileRenderDoesNotFreeInUseSurface();
#endif
    void injectedOomDegradesToCpuHandle();
};

#ifdef __APPLE__
void TestGpuMicrostress::evictWhileRenderDoesNotFreeInUseSurface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    std::weak_ptr<GpuSurface> weakSurface = surface;

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    FrameHandle cacheHandle = makeGpuFrameHandle(surface, rhi, meta);
    surface.reset();

    auto fence = DecodeDoneFence::create();
    QVERIFY(fence != nullptr);

    FrameHandle consumerHandle = cacheHandle;
    std::atomic<bool> waited{false};
    std::atomic<bool> readbackOk{false};
    std::thread consumer([&] {
        waited.store(fence->waitDecodeDone(2000), std::memory_order_release);
        if (!waited.load(std::memory_order_acquire)) return;
        readbackOk.store(consumerHandle.readToCpu(FramePixelFormat::Yuv420p).isValid(),
                         std::memory_order_release);
    });

    std::thread evictor([&] {
        fence->signalDecodeDone();
        cacheHandle = FrameHandle();
    });

    evictor.join();
    consumer.join();

    QVERIFY(waited.load(std::memory_order_acquire));
    QVERIFY(readbackOk.load(std::memory_order_acquire));
    QVERIFY(!weakSurface.expired());

    consumerHandle = FrameHandle();
    QVERIFY(weakSurface.expired());
}
#endif

void TestGpuMicrostress::injectedOomDegradesToCpuHandle() {
    gpuSetInjectedAllocFailures(1);

    FrameHandle handle;
#ifdef __APPLE__
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface == nullptr);
    handle = solidYuv420pHandle(64, 48, 16, 128, 128);
#else
    QVERIFY(gpuConsumeInjectedAllocFailure());
    handle = solidYuv420pHandle(64, 48, 16, 128, 128);
#endif

    gpuSetInjectedAllocFailures(0);
    QVERIFY(!handle.isNull());
    QVERIFY(!handle.isGpuBacked());
    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
}

QTEST_GUILESS_MAIN(TestGpuMicrostress)
#include "tst_gpu_microstress.moc"
