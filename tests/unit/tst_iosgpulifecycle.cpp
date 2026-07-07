// The iOS GPU lifecycle sink is the platform-neutral reaction to suspend/resume.
// It is unit-tested on the macOS host with no UIKit: background bumps the GPU
// generation and foreground clears the suspend flag.
#include <QtTest>

#include <QThread>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/iosgpulifecyclesink.h"
#include "playback/gpu/iosmemoryheadroom.h"

#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif

class TestIosGpuLifecycle : public QObject {
    Q_OBJECT
private slots:
    void init();
    void backgroundBumpsGenerationAndSuspends();
    void foregroundClearsSuspend();
    void memoryWarningCountsOnlyWhileForeground();
    void memoryHeadroomStubContract();
    void sinkRegistryRoundTrips();
    void presentBlockRunsOnceOnHost();
#ifdef __APPLE__
    void offscreenMetalRenderProducesPlanes();
#endif
};

void TestIosGpuLifecycle::init() {
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestIosGpuLifecycle::backgroundBumpsGenerationAndSuspends() {
    DefaultIosGpuLifecycleSink sink;
    const uint64_t before = GpuGenerationCounter::instance().current();
    QVERIFY(!sink.isSuspended());
    sink.onEnterBackground();
    QVERIFY(sink.isSuspended());
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(GpuGenerationCounter::instance().current() > before);
    QCOMPARE(sink.generationAtLastBackground(), GpuGenerationCounter::instance().current());
}

void TestIosGpuLifecycle::foregroundClearsSuspend() {
    DefaultIosGpuLifecycleSink sink;
    sink.onEnterBackground();
    QVERIFY(sink.isSuspended());
    sink.onEnterForeground();
    QVERIFY(!sink.isSuspended());
}

void TestIosGpuLifecycle::memoryWarningCountsOnlyWhileForeground() {
    DefaultIosGpuLifecycleSink sink;
    QCOMPARE(sink.memoryWarningCount(), uint64_t(0));

    sink.onMemoryWarning();
    QCOMPARE(sink.memoryWarningCount(), uint64_t(1));

    sink.onEnterBackground();
    sink.onMemoryWarning();
    QCOMPARE(sink.memoryWarningCount(), uint64_t(1));

    sink.onEnterForeground();
    sink.onMemoryWarning();
    QCOMPARE(sink.memoryWarningCount(), uint64_t(2));
}

void TestIosGpuLifecycle::memoryHeadroomStubContract() {
    const uint64_t bytes = iosAvailableMemoryBytes();
#if defined(Q_OS_IOS)
    Q_UNUSED(bytes);
#else
    QCOMPARE(bytes, uint64_t(0));
#endif
}

void TestIosGpuLifecycle::sinkRegistryRoundTrips() {
    DefaultIosGpuLifecycleSink sink;
    setIosGpuLifecycleSink(&sink);
    QCOMPARE(iosGpuLifecycleSink(), static_cast<IosGpuLifecycleSink*>(&sink));
    setIosGpuLifecycleSink(nullptr);
    QVERIFY(iosGpuLifecycleSink() != &sink);
}

void TestIosGpuLifecycle::presentBlockRunsOnceOnHost() {
    auto ctx = GpuRhiContext::createNullForTest();
    QVERIFY(ctx);
    int ran = 0;
    QThread* callerThread = QThread::currentThread();
    QThread* blockThread = nullptr;
    ctx->presentOnMainThread([&] {
        ++ran;
        blockThread = QThread::currentThread();
    });
    QCOMPARE(ran, 1);
    QCOMPARE(blockThread, callerThread);
}

#ifdef __APPLE__
void TestIosGpuLifecycle::offscreenMetalRenderProducesPlanes() {
    // The automated iOS proxy is the shared QRhi::Metal path on the macOS host.
    // Physical-device presentation, thermal behavior, and suspend/resume remain manual.
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no Metal device on this host; on-device render validation is manual");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    const CpuPlanes planes = ctx->importAndReadback(surface, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 64);
    QCOMPARE(planes.height, 48);
    QVERIFY(planes.isValid());
}
#endif

QTEST_GUILESS_MAIN(TestIosGpuLifecycle)
#include "tst_iosgpulifecycle.moc"
