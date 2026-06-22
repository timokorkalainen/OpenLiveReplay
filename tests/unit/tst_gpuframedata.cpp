// GpuFrameData is the GPU concrete of IFrameData: it reports GPU residency,
// exposes its surface, and downloads lazily through GpuRhiContext.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpuframeretirequeue.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "playback/output/gpureadbacktelemetry.h"
#include "playback/output/outputframecache.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/vtkeepsurfaceimporter.h"
#endif

class TestGpuFrameData : public QObject {
    Q_OBJECT
private slots:
    void gpuBackedReportsSurface();
    void completedReadbackRetainReleasesImmediately();
#ifdef __APPLE__
    void gpuPresentabilityDoesNotReadBack();
    void outputCacheInsertionDoesNotReadBack();
    void readToCpuDownloadsAndCounts();
    void readbackMatchesCpuWithinOneLsb();
    void importVtBufferProducesGpuHandle();
    void readbackStampsSurfacePendingFence();
    void readbackRetainsSurfaceWhenEvictionSawNoPendingFence();
#endif
};

namespace {

class DeferredFence final : public GpuFence {
public:
    uint64_t signal() override { return m_next.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override { return completedValue() >= value; }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

private:
    std::atomic<uint64_t> m_next{0};
    std::atomic<uint64_t> m_completed{0};
};

class TestSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
    void retainUntilFenceRetired(uint64_t fenceValue) override {
        m_pending.store(fenceValue, std::memory_order_release);
    }
    uint64_t pendingFenceValue() const override {
        return m_pending.load(std::memory_order_acquire);
    }

private:
    std::atomic<uint64_t> m_pending{0};
};

} // namespace

void TestGpuFrameData::gpuBackedReportsSurface() {
#ifdef __APPLE__
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    QVERIFY(handle.isGpuBacked());
    QVERIFY(handle.data()->gpuSurface() != nullptr);
    QCOMPARE(handle.data()->nativeFormat(), FramePixelFormat::Nv12);
#else
    QSKIP("GPU backend is Apple-only in Phase 2");
#endif
}

void TestGpuFrameData::completedReadbackRetainReleasesImmediately() {
    gpuDrainCompletedReadbackRetains();

    auto fence = std::make_shared<DeferredFence>();
    fence->complete(1);
    auto surface = std::make_shared<TestSurface>();
    std::weak_ptr<GpuSurface> weakSurface = surface;

    gpuRetainSurfaceUntilFenceRetired(surface, fence, 1);
    surface.reset();

    QVERIFY2(weakSurface.expired(),
             "already-completed readback fences must not leave a surface retained until a later "
             "opportunistic drain");
    QCOMPARE(gpuPendingReadbackRetainCount(), qsizetype(0));
}

#ifdef __APPLE__
void TestGpuFrameData::gpuPresentabilityDoesNotReadBack() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 40;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);
    QVERIFY(handle.isPresentable());
    QCOMPARE(data->readToCpuCount(), 0);
}

void TestGpuFrameData::outputCacheInsertionDoesNotReadBack() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 40;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);

    OutputFrameCache cache(1, 64, 48);
    cache.insertVideoFrame(handle);
    QCOMPARE(data->readToCpuCount(), 0);

    auto cached = cache.videoFrameAt(0, 40);
    QVERIFY(cached.has_value());
    QVERIFY(cached->isGpuBacked());
}

void TestGpuFrameData::readToCpuDownloadsAndCounts() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    GpuReadbackTelemetry::instance().reset();
    gpuResetFrameReadToCpuCount();

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const auto* data = dynamic_cast<const GpuFrameData*>(handle.data());
    QVERIFY(data != nullptr);
    QCOMPARE(data->readToCpuCount(), 0);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(0));
    const CpuPlanes planes = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(planes.isValid());
    QCOMPARE(planes.width, 64);
    QCOMPARE(planes.height, 48);
    QCOMPARE(data->readToCpuCount(), 1);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(1));

    const GpuReadbackTelemetrySnapshot once = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(once.gpuReadbacks, qint64(1));
    QCOMPARE(once.redundantReadbacks, qint64(0));

    const CpuPlanes second = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(second.isValid());
    QCOMPARE(data->readToCpuCount(), 1);
    QCOMPARE(gpuFrameReadToCpuCount(), qint64(1));

    const GpuReadbackTelemetrySnapshot twice = GpuReadbackTelemetry::instance().snapshot();
    QCOMPARE(twice.gpuReadbacks, qint64(1));
    QCOMPARE(twice.redundantReadbacks, qint64(0));
}

void TestGpuFrameData::readbackMatchesCpuWithinOneLsb() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(16, 16);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 16;
    meta.key.height = 16;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta);
    const CpuPlanes got = handle.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(got.isValid());
    for (char b : got.plane[0])
        QVERIFY(qAbs(static_cast<int>(static_cast<uchar>(b))) <= 1);
    for (char b : got.plane[1])
        QVERIFY(qAbs(static_cast<int>(static_cast<uchar>(b))) <= 1);
    for (char b : got.plane[2])
        QVERIFY(qAbs(static_cast<int>(static_cast<uchar>(b))) <= 1);
}

void TestGpuFrameData::importVtBufferProducesGpuHandle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.key.ptsMs = 40;

    FrameHandle handle = importVtSurface(surface, meta, rhi);
    QVERIFY(handle.isGpuBacked());
    QCOMPARE(handle.metadata().key.format, FramePixelFormat::Nv12);
    QCOMPARE(handle.metadata().key.width, 64);
    QCOMPARE(handle.metadata().key.height, 48);
}

void TestGpuFrameData::readbackStampsSurfacePendingFence() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto renderFence = rhi->createFence();
    QVERIFY(renderFence != nullptr);

    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta, renderFence);
    QCOMPARE(surface->pendingFenceValue(), uint64_t(0));
    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    QVERIFY(surface->pendingFenceValue() >= uint64_t(1));
}

void TestGpuFrameData::readbackRetainsSurfaceWhenEvictionSawNoPendingFence() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto renderFence = std::make_shared<DeferredFence>();
    auto surface = makeAppleNv12Surface(64, 48);
    QVERIFY(surface != nullptr);
    std::weak_ptr<GpuSurface> weakSurface = surface;

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    FrameHandle handle = makeGpuFrameHandle(surface, rhi, meta, renderFence);
    surface.reset();

    GpuFrameRetireQueue preReadbackRetireQueue;
    preReadbackRetireQueue.collect(handle, renderFence);
    QCOMPARE(preReadbackRetireQueue.size(), 0);

    QVERIFY(handle.readToCpu(FramePixelFormat::Yuv420p).isValid());
    QVERIFY(!weakSurface.expired());
    handle = FrameHandle();
    QVERIFY(!weakSurface.expired());
    QVERIFY(gpuPendingReadbackRetainCount() >= 1);

    renderFence->complete(1);
    gpuDrainCompletedReadbackRetains();
    QVERIFY(weakSurface.expired());
}
#endif

QTEST_GUILESS_MAIN(TestGpuFrameData)
#include "tst_gpuframedata.moc"
