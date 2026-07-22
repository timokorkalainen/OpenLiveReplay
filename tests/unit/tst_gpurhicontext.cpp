// GpuRhiContext owns one QRhi on a dedicated render thread. On a GPU host it can
// import an IOSurface-backed surface and read it back; without a backend it
// returns nullptr so callers can degrade to CPU.
#include <QtTest>

#include <QProcess>
#include <QProcessEnvironment>

#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/framepixelformat.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

#include <array>
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
    void renderThreadExceptionsReturnFailureAndRemainUsable();
    void renderThreadCallbackMayReleaseFinalContextOwner();
    void readbackFenceIsInitializedBeforePublication();
    void readbackFenceCreationAllowsNestedRenderEntry();
    void failedEagerReadbackFenceCreationIsTerminal();
#ifdef __APPLE__
    void surfaceCompatibilityTracksBoundFenceAndFailsClosed();
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
    CVPixelBufferRef pb = retainApplePixelBufferWrapper(surface);
    if (!pb) return false;

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
    CVPixelBufferRef pb = retainApplePixelBufferWrapper(surface);
    if (!pb) return false;

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
    const GpuReadbackResult readback = GpuRhiContextTestAuthority::importAndReadback(
        ctx, std::make_shared<TestSurface>(&validChecks), FramePixelFormat::Yuv420p);
    QVERIFY(!readback.planes.isValid());
    QCOMPARE(readback.outcome, GpuSubmitOutcome::NotSubmitted);
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
        return GpuRhiContextTestAuthority::importAndReadback(
            ctx, std::make_shared<TestSurface>(nullptr), FramePixelFormat::Yuv420p);
    });
    const bool returnedImmediately =
        readback.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;

    release.store(true, std::memory_order_release);
    blocker.join();
    if (returnedImmediately) {
        const GpuReadbackResult result = readback.get();
        QVERIFY(!result.planes.isValid());
        QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    }
    QVERIFY(returnedImmediately);
}

void TestGpuRhiContext::renderThreadExceptionsReturnFailureAndRemainUsable() {
    constexpr auto childEnvironment = "OLR_GPU_RHI_THROWING_INVOKE_CHILD";
    if (!qEnvironmentVariableIsSet(childEnvironment)) {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromLatin1(childEnvironment), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("renderThreadExceptionsReturnFailureAndRemainUsable")});

        QVERIFY(child.waitForStarted(5000));
        const bool finished = child.waitForFinished(10000);
        if (!finished) {
            child.kill();
            (void) child.waitForFinished(5000);
        }
        const QByteArray output = child.readAll();
        QVERIFY2(finished, output.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }

    auto ctx = testContext();
    if (!ctx) QSKIP("no RHI backend on this host");

    QVERIFY(!ctx->invokeOnRenderThread([](QRhi*) { throw 0; }));
    bool externalRecovery = false;
    QVERIFY(ctx->invokeOnRenderThread([&](QRhi*) { externalRecovery = true; }));
    QVERIFY(externalRecovery);

    bool inlineFailure = true;
    bool inlineRecovery = false;
    QVERIFY(ctx->invokeOnRenderThread([&](QRhi*) {
        inlineFailure = ctx->invokeOnRenderThread([](QRhi*) { throw 0; });
        inlineRecovery = ctx->invokeOnRenderThread([](QRhi*) {});
    }));
    QVERIFY(!inlineFailure);
    QVERIFY(inlineRecovery);
}

void TestGpuRhiContext::renderThreadCallbackMayReleaseFinalContextOwner() {
    constexpr auto childEnvironment = "OLR_GPU_RHI_FINAL_OWNER_CHILD";
    if (!qEnvironmentVariableIsSet(childEnvironment)) {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromLatin1(childEnvironment), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("renderThreadCallbackMayReleaseFinalContextOwner")});

        QVERIFY(child.waitForStarted(5000));
        const bool finished = child.waitForFinished(10000);
        if (!finished) {
            child.kill();
            (void) child.waitForFinished(5000);
        }
        const QByteArray output = child.readAll();
        QVERIFY2(finished, output.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }

    auto ctx = testContext();
    if (!ctx) QSKIP("no RHI backend on this host");
    std::weak_ptr<GpuRhiContext> weakContext = ctx;
    bool callbackRan = false;

    QVERIFY(ctx->invokeOnRenderThread([owner = std::move(ctx), &callbackRan](QRhi*) mutable {
        callbackRan = true;
        owner.reset();
    }));

    QVERIFY(callbackRan);
    QVERIFY(weakContext.expired());
}

void TestGpuRhiContext::readbackFenceIsInitializedBeforePublication() {
    auto ctx = testContext();
    if (!ctx) QSKIP("no RHI backend on this host");

    const std::shared_ptr<GpuFence> publishedFence =
        GpuRhiContextTestAuthority::readbackFenceForTest(ctx);
    QVERIFY(publishedFence != nullptr);
    QCOMPARE(GpuRhiContextTestAuthority::readbackFenceInitializationAttemptsForTest(ctx), 1);

    constexpr int callerCount = 8;
    std::array<std::future<std::shared_ptr<GpuFence>>, callerCount> callers;
    for (auto& caller : callers) {
        caller = std::async(std::launch::async, [ctx] {
            return GpuRhiContextTestAuthority::readbackFenceForTest(ctx);
        });
    }
    for (auto& caller : callers)
        QCOMPARE(caller.get(), publishedFence);
    QCOMPARE(GpuRhiContextTestAuthority::readbackFenceInitializationAttemptsForTest(ctx), 1);
}

void TestGpuRhiContext::readbackFenceCreationAllowsNestedRenderEntry() {
    constexpr auto childEnvironment = "OLR_GPU_RHI_NESTED_READBACK_CHILD";
    if (!qEnvironmentVariableIsSet(childEnvironment)) {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromLatin1(childEnvironment), QStringLiteral("1"));
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("readbackFenceCreationAllowsNestedRenderEntry")});

        QVERIFY(child.waitForStarted(5000));
        const bool finished = child.waitForFinished(10000);
        if (!finished) {
            child.kill();
            (void) child.waitForFinished(5000);
        }
        const QByteArray output = child.readAll();
        QVERIFY2(finished, output.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
        return;
    }

    auto ctx = testContext();
    if (!ctx) QSKIP("no RHI backend on this host");
#ifdef __APPLE__
    std::shared_ptr<GpuSurface> surface = makeAppleNv12Surface(4, 2, ctx->surfaceCompatibility());
    QVERIFY(surface != nullptr);
#else
    auto surface = std::make_shared<TestSurface>(nullptr);
#endif
    bool nestedReturned = false;
    const bool invoked = ctx->invokeOnRenderThread([&](QRhi*) {
        (void) submitGpuReadback(ctx, surface, FramePixelFormat::Yuv420p);
        (void) GpuRhiContextTestAuthority::importAndReadback(ctx, surface,
                                                             FramePixelFormat::Yuv420p);
        nestedReturned = true;
    });

    QVERIFY(invoked);
    QVERIFY(nestedReturned);
    QVERIFY(GpuRhiContextTestAuthority::readbackFenceForTest(ctx) != nullptr);
}

void TestGpuRhiContext::failedEagerReadbackFenceCreationIsTerminal() {
    auto ctx = GpuRhiContext::createReadbackFenceFailureForTest();
    QVERIFY(ctx != nullptr);
    QVERIFY(ctx->isValid());
    auto surface = std::make_shared<TestSurface>(nullptr);

    (void) submitGpuReadback(ctx, surface, FramePixelFormat::Yuv420p);
    (void) submitGpuReadback(ctx, surface, FramePixelFormat::Yuv420p);

    QCOMPARE(GpuRhiContextTestAuthority::readbackFenceInitializationAttemptsForTest(ctx), 1);
    QCOMPARE(GpuRhiContextTestAuthority::injectedReadbackFenceFactoryCallsForTest(ctx), 1);
    QVERIFY(GpuRhiContextTestAuthority::readbackFenceForTest(ctx) == nullptr);
}

#ifdef __APPLE__
void TestGpuRhiContext::surfaceCompatibilityTracksBoundFenceAndFailsClosed() {
    auto nullContext = GpuRhiContext::createNullForTest();
    QVERIFY(nullContext != nullptr);
    QVERIFY(nullContext->isValid());
    QVERIFY(nullContext->isNullBackend());
    QVERIFY(!nullContext->isGpuBacked());
    const GpuSurfaceCompatibility nullCompatibility = nullContext->surfaceCompatibility();
    QCOMPARE(nullCompatibility.deviceDomainId, uintptr_t(0));
    QCOMPARE(nullCompatibility.authorityEpoch, uint64_t(0));

    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");
    const auto fence = GpuRhiContextTestAuthority::readbackFenceForTest(ctx);
    QVERIFY(fence != nullptr);
    const GpuFenceIdentity identity = fence->identity();
    const GpuSurfaceCompatibility compatibility = ctx->surfaceCompatibility();
    QCOMPARE(compatibility.deviceDomainId, identity.deviceDomainId);
    QCOMPARE(compatibility.authorityEpoch, identity.authorityEpoch);

    auto missingFence = GpuRhiContext::createReadbackFenceFailureForTest();
    QVERIFY(missingFence != nullptr);
    const GpuSurfaceCompatibility missing = missingFence->surfaceCompatibility();
    QCOMPARE(missing.deviceDomainId, uintptr_t(0));
    QCOMPARE(missing.authorityEpoch, uint64_t(0));
}

void TestGpuRhiContext::importAndReadbackProducesPlanes() {
    auto ctx = GpuRhiContext::create();
    if (!ctx) QSKIP("no RHI backend on this host");

    auto surface = makeAppleNv12Surface(4, 2, ctx->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    QVERIFY(fillNv12Surface(surface));
    const int before = ctx->rhiReadbackCountForTest();
    const GpuReadbackResult readback =
        GpuRhiContextTestAuthority::importAndReadback(ctx, surface, FramePixelFormat::Yuv420p);
    const CpuPlanes& planes = readback.planes;
    const int after = ctx->rhiReadbackCountForTest();
    QCOMPARE(planes.format, FramePixelFormat::Yuv420p);
    QCOMPARE(planes.width, 4);
    QCOMPARE(planes.height, 2);
    QVERIFY(planes.isValid());
    QCOMPARE(readback.outcome, GpuSubmitOutcome::Submitted);
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

    auto surface = makeAppleRgba8Surface(2, 1, ctx->surfaceCompatibility());
    QVERIFY(surface != nullptr);
    QVERIFY(fillRgbaSurfaceBgra(surface));

    const int before = ctx->rhiReadbackCountForTest();
    const GpuReadbackResult readback =
        GpuRhiContextTestAuthority::importAndReadback(ctx, surface, FramePixelFormat::Rgba8);
    const CpuPlanes& planes = readback.planes;
    const int after = ctx->rhiReadbackCountForTest();

    QCOMPARE(planes.format, FramePixelFormat::Rgba8);
    QCOMPARE(planes.width, 2);
    QCOMPARE(planes.height, 1);
    QVERIFY(planes.isValid());
    QCOMPARE(readback.outcome, GpuSubmitOutcome::Submitted);
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
