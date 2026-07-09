// GpuRhiContext owns one QRhi on a dedicated render thread. On a GPU host it can
// import an IOSurface-backed surface and read it back; without a backend it
// returns nullptr so callers can degrade to CPU.
#include <QtTest>

#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framepixelformat.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

class TestGpuRhiContext : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void deviceLostStartsFalseThenLatchesOnInject();
    void lostContextReadbackShortCircuitsRenderThread();
#ifdef __APPLE__
    void importAndReadbackProducesPlanes();
    void importAndReadbackUsesRhiForRgbaSurface();
#endif
};

namespace {

class TestSurface final : public GpuSurface {
public:
    explicit TestSurface(std::atomic<int>* validChecks) : m_validChecks(validChecks) {}

    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override {
        if (m_validChecks) m_validChecks->fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    void* nativeHandle() const override { return nullptr; }

private:
    std::atomic<int>* m_validChecks = nullptr;
};

std::shared_ptr<GpuRhiContext> testContext() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) ctx = GpuRhiContext::createNullForTest();
    if (!ctx) ctx = GpuRhiContext::createWarpForTest();
    return ctx;
}

#ifdef __APPLE__
bool fillRgbaSurfaceBgra(const std::shared_ptr<GpuSurface>& surface) {
    if (!surface) return false;
    auto ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    if (!ioSurface) return false;

    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pb) !=
            kCVReturnSuccess ||
        !pb) {
        return false;
    }

    bool ok = false;
    if (CVPixelBufferLockBaseAddress(pb, 0) == kCVReturnSuccess) {
        auto* base = static_cast<uchar*>(CVPixelBufferGetBaseAddress(pb));
        const size_t stride = CVPixelBufferGetBytesPerRow(pb);
        if (base && stride >= 8) {
            base[0] = 0x30; // B
            base[1] = 0x20; // G
            base[2] = 0x10; // R
            base[3] = 0xff; // A
            base[4] = 0x03;
            base[5] = 0x02;
            base[6] = 0x01;
            base[7] = 0x80;
            ok = true;
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);
    }
    CVPixelBufferRelease(pb);
    return ok;
}

bool fillNv12Surface(const std::shared_ptr<GpuSurface>& surface) {
    if (!surface) return false;
    auto ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    if (!ioSurface) return false;

    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pb) !=
            kCVReturnSuccess ||
        !pb) {
        return false;
    }

    bool ok = false;
    if (CVPixelBufferLockBaseAddress(pb, 0) == kCVReturnSuccess) {
        auto* y = static_cast<uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
        auto* uv = static_cast<uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
        const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        if (y && uv && yStride >= 4 && uvStride >= 4) {
            for (int row = 0; row < 2; ++row) {
                for (int x = 0; x < 4; ++x) {
                    y[static_cast<size_t>(row) * yStride + static_cast<size_t>(x)] =
                        uchar(0x10 + row * 4 + x);
                }
            }
            uv[0] = 0x80;
            uv[1] = 0x90;
            uv[2] = 0x81;
            uv[3] = 0x91;
            ok = true;
        }
        CVPixelBufferUnlockBaseAddress(pb, 0);
    }
    CVPixelBufferRelease(pb);
    return ok;
}
#endif

} // namespace

void TestGpuRhiContext::createIsNullOrValidNeverPartial() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");
    QVERIFY(ctx->isValid());
}

void TestGpuRhiContext::deviceLostStartsFalseThenLatchesOnInject() {
    auto ctx = testContext();
    if (!ctx) QSKIP("no RHI backend on this host");
    QVERIFY(!ctx->deviceLost());
    ctx->injectDeviceLostForTest();
    QVERIFY(ctx->deviceLost());

    std::atomic<int> validChecks{0};
    const CpuPlanes planes = ctx->importAndReadback(std::make_shared<TestSurface>(&validChecks),
                                                    FramePixelFormat::Yuv420p);
    QVERIFY(!planes.isValid());
    QCOMPARE(validChecks.load(std::memory_order_acquire), 0);
}

void TestGpuRhiContext::lostContextReadbackShortCircuitsRenderThread() {
    auto ctx = testContext();
    if (!ctx) QSKIP("no RHI backend on this host");
    ctx->injectDeviceLostForTest();

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread blocker([&] {
        ctx->invokeOnRenderThread([&](QRhi*) {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    });

    QTRY_VERIFY(entered.load(std::memory_order_acquire));
    auto readback = std::async(std::launch::async, [&] {
        return ctx->importAndReadback(std::make_shared<TestSurface>(nullptr),
                                      FramePixelFormat::Yuv420p);
    });
    const bool returnedImmediately =
        readback.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;

    release.store(true, std::memory_order_release);
    blocker.join();
    if (returnedImmediately) {
        QVERIFY(!readback.get().isValid());
    }
    QVERIFY(returnedImmediately);
}

#ifdef __APPLE__
void TestGpuRhiContext::importAndReadbackProducesPlanes() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");

    auto surface = makeAppleNv12Surface(4, 2);
    QVERIFY(surface != nullptr);
    QVERIFY(fillNv12Surface(surface));
    const int before = ctx->rhiReadbackCountForTest();
    const CpuPlanes planes = ctx->importAndReadback(surface, FramePixelFormat::Yuv420p);
    const int after = ctx->rhiReadbackCountForTest();
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 4);
    QCOMPARE(planes.height, 2);
    QVERIFY(planes.isValid());
    QCOMPARE(static_cast<uchar>(planes.plane[0][0]), uchar(0x10));
    QCOMPARE(static_cast<uchar>(planes.plane[0][1]), uchar(0x11));
    QCOMPARE(static_cast<uchar>(planes.plane[0][4]), uchar(0x14));
    QCOMPARE(static_cast<uchar>(planes.plane[1][0]), uchar(0x80));
    QCOMPARE(static_cast<uchar>(planes.plane[1][1]), uchar(0x81));
    QCOMPARE(static_cast<uchar>(planes.plane[2][0]), uchar(0x90));
    QCOMPARE(static_cast<uchar>(planes.plane[2][1]), uchar(0x91));
    QVERIFY(after > before);
}

void TestGpuRhiContext::importAndReadbackUsesRhiForRgbaSurface() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");

    auto surface = makeAppleRgba8Surface(2, 1);
    QVERIFY(surface != nullptr);
    QVERIFY(fillRgbaSurfaceBgra(surface));

    const int before = ctx->rhiReadbackCountForTest();
    const CpuPlanes planes = ctx->importAndReadback(surface, FramePixelFormat::Rgba8);
    const int after = ctx->rhiReadbackCountForTest();

    QCOMPARE(planes.format, FramePixelFormat::Rgba8);
    QCOMPARE(planes.width, 2);
    QCOMPARE(planes.height, 1);
    QVERIFY(planes.isValid());
    QVERIFY(planes.plane[0].size() >= 8);
    QCOMPARE(static_cast<uchar>(planes.plane[0][0]), uchar(0x10));
    QCOMPARE(static_cast<uchar>(planes.plane[0][1]), uchar(0x20));
    QCOMPARE(static_cast<uchar>(planes.plane[0][2]), uchar(0x30));
    QCOMPARE(static_cast<uchar>(planes.plane[0][3]), uchar(0xff));
    QCOMPARE(static_cast<uchar>(planes.plane[0][4]), uchar(0x01));
    QCOMPARE(static_cast<uchar>(planes.plane[0][5]), uchar(0x02));
    QCOMPARE(static_cast<uchar>(planes.plane[0][6]), uchar(0x03));
    QCOMPARE(static_cast<uchar>(planes.plane[0][7]), uchar(0x80));
    QVERIFY(after > before);
}
#endif

QTEST_GUILESS_MAIN(TestGpuRhiContext)
#include "tst_gpurhicontext.moc"
