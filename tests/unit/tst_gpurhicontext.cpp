// GpuRhiContext owns one QRhi on a dedicated render thread. On a GPU host it can
// import an IOSurface-backed surface and read it back; without a backend it
// returns nullptr so callers can degrade to CPU.
#include <QtTest>

#include "playback/gpu/gpurhicontext.h"
#include "playback/output/framepixelformat.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif

class TestGpuRhiContext : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
#ifdef __APPLE__
    void importAndReadbackProducesPlanes();
#endif
};

void TestGpuRhiContext::createIsNullOrValidNeverPartial() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");
    QVERIFY(ctx->isValid());
}

#ifdef __APPLE__
void TestGpuRhiContext::importAndReadbackProducesPlanes() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    const CpuPlanes planes = ctx->importAndReadback(surface, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 64);
    QCOMPARE(planes.height, 48);
    QVERIFY(planes.isValid());
}
#endif

QTEST_GUILESS_MAIN(TestGpuRhiContext)
#include "tst_gpurhicontext.moc"
