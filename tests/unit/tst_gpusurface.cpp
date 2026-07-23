// GpuSurface is the concrete type behind the Phase-1 opaque forward declaration.
// The shared header must stay platform-neutral, so this test compiles it without
// Apple, Metal, or D3D headers and exercises the desc/validity contract.
#include <QtTest>

#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framepixelformat.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/gpurhicontext.h"
#endif
#endif

namespace {
class FakeGpuSurface : public GpuSurface {
public:
    FakeGpuSurface(GpuSurfaceDesc desc, bool valid) : m_desc(desc), m_valid(valid) {}

    GpuSurfaceDesc desc() const override { return m_desc; }
    bool isValid() const override { return m_valid; }
    void* nativeHandle() const override {
        return m_valid ? const_cast<FakeGpuSurface*>(this) : nullptr;
    }

private:
    GpuSurfaceDesc m_desc;
    bool m_valid = false;
};
} // namespace

class TestGpuSurface : public QObject {
    Q_OBJECT
private slots:
    void descRoundTrips();
    void invalidSurfaceHasNullHandle();
    void pipelineFlagDefaultsOff();
    void injectedAllocFailureIsOneShot();
#ifdef __APPLE__
    void appleSurfaceIsIoSurfaceBacked();
    void appleSurfaceRespectsInjectedAllocFailure();
    void appleSurfaceTracksPendingFence();
#ifdef OLR_GPU_PIPELINE_BUILD
    void appleSurfaceSeparatesFrameGenerationFromDeviceAuthority();
#endif
    void applePixelBufferWrapperOutlivesSurfaceOwner();
#endif
};

void TestGpuSurface::descRoundTrips() {
    FakeGpuSurface s({FramePixelFormat::Nv12, 1920, 1080}, true);
    QCOMPARE(s.desc().format, FramePixelFormat::Nv12);
    QCOMPARE(s.desc().width, 1920);
    QCOMPARE(s.desc().height, 1080);
    QVERIFY(s.isValid());
    QVERIFY(s.nativeHandle() != nullptr);
}

void TestGpuSurface::invalidSurfaceHasNullHandle() {
    FakeGpuSurface s({FramePixelFormat::Nv12, 0, 0}, false);
    QVERIFY(!s.isValid());
    QVERIFY(s.nativeHandle() == nullptr);
}

void TestGpuSurface::pipelineFlagDefaultsOff() {
    qunsetenv("OLR_GPU_PIPELINE");
    QVERIFY(!gpuPipelineEnabled());
    qputenv("OLR_GPU_PIPELINE", "1");
    QVERIFY(gpuPipelineEnabled());
    qputenv("OLR_GPU_PIPELINE", "true");
    QVERIFY(gpuPipelineEnabled());
    qputenv("OLR_GPU_PIPELINE", "on");
    QVERIFY(gpuPipelineEnabled());
    qputenv("OLR_GPU_PIPELINE", "0");
    QVERIFY(!gpuPipelineEnabled());
    qunsetenv("OLR_GPU_PIPELINE");
}

void TestGpuSurface::injectedAllocFailureIsOneShot() {
    gpuSetInjectedAllocFailures(2);
    QVERIFY(gpuConsumeInjectedAllocFailure());
    QVERIFY(gpuConsumeInjectedAllocFailure());
    QVERIFY(!gpuConsumeInjectedAllocFailure());
}

#ifdef __APPLE__
void TestGpuSurface::appleSurfaceIsIoSurfaceBacked() {
    auto s = makeAppleNv12Surface(64, 48);
    QVERIFY(s != nullptr);
    QVERIFY(s->isValid());
    QCOMPARE(s->desc().format, FramePixelFormat::Nv12);
    QCOMPARE(s->desc().width, 64);
    QCOMPARE(s->desc().height, 48);
    QVERIFY(s->hasNativeBacking());
}

void TestGpuSurface::appleSurfaceRespectsInjectedAllocFailure() {
    gpuSetInjectedAllocFailures(1);
    auto fail = makeAppleNv12Surface(64, 48);
    QVERIFY(fail == nullptr);
    auto ok = makeAppleNv12Surface(64, 48);
    QVERIFY(ok != nullptr);
}

void TestGpuSurface::appleSurfaceTracksPendingFence() {
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(0));
    surface->retainUntilFenceRetired(7);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(7));
    surface->retainUntilFenceRetired(3);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(7));
    surface->retainUntilFenceRetired(9);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(9));
}

#ifdef OLR_GPU_PIPELINE_BUILD
void TestGpuSurface::appleSurfaceSeparatesFrameGenerationFromDeviceAuthority() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const auto resetState = qScopeGuard([&] {
        monitor.reset();
        GpuGenerationCounter::instance().resetForTest();
    });
    auto oldRhi = GpuRhiContext::create();
    if (!oldRhi) QSKIP("no Metal RHI context on this host");
    auto oldSurface = makeAppleNv12Surface(64, 48, oldRhi->surfaceCompatibility());
    auto oldFence = oldRhi->createFence();
    if (!oldSurface || !oldFence) QSKIP("no Metal device/fence on this host");
    QVERIFY(oldFence->sharesDeviceAuthorityWith(oldSurface));

    GpuGenerationCounter::instance().bump();
    auto postSeekFence = oldRhi->createFence();
    auto postSeekSurface = makeAppleNv12Surface(64, 48, oldRhi->surfaceCompatibility());
    QVERIFY(postSeekFence != nullptr);
    QVERIFY(postSeekSurface != nullptr);
    QCOMPARE(postSeekFence->identity().deviceDomainId, oldFence->identity().deviceDomainId);
    QCOMPARE(postSeekFence->identity().authorityEpoch, oldFence->identity().authorityEpoch);
    QVERIFY(postSeekFence->sharesDeviceAuthorityWith(oldSurface));
    QVERIFY(postSeekFence->sharesDeviceAuthorityWith(postSeekSurface));

    monitor.recordLoss();
    monitor.beginRebuild();
    auto rebuiltRhi = GpuRhiContext::create();
    QVERIFY(rebuiltRhi != nullptr);
    auto rebuiltFence = rebuiltRhi->createFence();
    auto rebuiltSurface = makeAppleNv12Surface(64, 48, rebuiltRhi->surfaceCompatibility());
    QVERIFY(rebuiltFence != nullptr);
    QVERIFY(rebuiltSurface != nullptr);
    QCOMPARE(rebuiltFence->identity().deviceDomainId, oldFence->identity().deviceDomainId);
    QVERIFY(rebuiltFence->identity().authorityEpoch != oldFence->identity().authorityEpoch);
    QVERIFY(!rebuiltFence->sharesDeviceAuthorityWith(oldSurface));
    QVERIFY(rebuiltFence->sharesDeviceAuthorityWith(rebuiltSurface));
}
#endif

void TestGpuSurface::applePixelBufferWrapperOutlivesSurfaceOwner() {
    auto surface = makeAppleNv12Surface(64, 48);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");
    CVPixelBufferRef wrapper = retainApplePixelBufferWrapper(surface);
    QVERIFY(wrapper != nullptr);
    surface.reset();
    QCOMPARE(CVPixelBufferGetWidth(wrapper), size_t(64));
    QCOMPARE(CVPixelBufferGetHeight(wrapper), size_t(48));
    QVERIFY(CVPixelBufferGetIOSurface(wrapper) != nullptr);
    CVPixelBufferRelease(wrapper);
}
#endif

QTEST_GUILESS_MAIN(TestGpuSurface)
#include "tst_gpusurface.moc"
