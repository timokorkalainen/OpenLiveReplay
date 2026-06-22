#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframeretirequeue.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <memory>
#include <utility>

class FakeGpuSurface final : public GpuSurface {
public:
    explicit FakeGpuSurface(uint64_t pendingFence) : m_pendingFence(pendingFence) {}

    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 4, 4}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    uint64_t m_pendingFence = 0;
};

class FakeGpuFrameData final : public IFrameData {
public:
    explicit FakeGpuFrameData(std::shared_ptr<FakeGpuSurface> surface)
        : m_surface(std::move(surface)) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override { return {}; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<FakeGpuSurface> m_surface;
};

class FakeFence final : public GpuFence {
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

static FrameHandle makeGpuFrame(uint64_t pendingFence) {
    auto surface = std::make_shared<FakeGpuSurface>(pendingFence);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = qint64(pendingFence);
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    return FrameHandle(std::make_shared<FakeGpuFrameData>(std::move(surface)), meta);
}

class TestEvictionGuard : public QObject {
    Q_OBJECT
private slots:
    void ignoresCpuAndUnfencedGpuFrames();
    void timeoutRetainsFrameAndCountsStall();
    void completedFenceReleasesFrame();
    void drainBudgetLimitsFenceWaitsPerPass();
};

void TestEvictionGuard::ignoresCpuAndUnfencedGpuFrames() {
    GpuFrameRetireQueue queue;
    auto fence = std::make_shared<FakeFence>();

    queue.collect(solidYuv420pHandle(4, 4, 16, 128, 128), fence);
    queue.collect(makeGpuFrame(0), fence);

    QCOMPARE(queue.size(), 0);
}

void TestEvictionGuard::timeoutRetainsFrameAndCountsStall() {
    auto data = std::static_pointer_cast<const IFrameData>(
        std::make_shared<FakeGpuFrameData>(std::make_shared<FakeGpuSurface>(7)));
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 7;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    std::weak_ptr<const IFrameData> weakData = data;

    GpuFrameRetireQueue queue;
    auto fence = std::make_shared<FakeFence>();
    queue.collect(FrameHandle(std::move(data), meta), fence);

    int stalls = 0;
    QCOMPARE(queue.drain(0, &stalls), 0);

    QCOMPARE(stalls, 1);
    QCOMPARE(fence->waitCalls, 1);
    QCOMPARE(queue.size(), 1);
    QVERIFY(!weakData.expired());
}

void TestEvictionGuard::completedFenceReleasesFrame() {
    auto data = std::static_pointer_cast<const IFrameData>(
        std::make_shared<FakeGpuFrameData>(std::make_shared<FakeGpuSurface>(4)));
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 4;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    std::weak_ptr<const IFrameData> weakData = data;

    GpuFrameRetireQueue queue;
    auto fence = std::make_shared<FakeFence>();
    queue.collect(FrameHandle(std::move(data), meta), fence);

    fence->complete(4);
    int stalls = 0;
    QCOMPARE(queue.drain(0, &stalls), 1);

    QCOMPARE(stalls, 0);
    QCOMPARE(queue.size(), 0);
    QVERIFY(weakData.expired());
}

void TestEvictionGuard::drainBudgetLimitsFenceWaitsPerPass() {
    GpuFrameRetireQueue queue;
    auto fence = std::make_shared<FakeFence>();
    queue.collect(makeGpuFrame(4), fence);
    queue.collect(makeGpuFrame(5), fence);

    int stalls = 0;
    QCOMPARE(queue.drain(1, &stalls, 1), 0);

    QCOMPARE(fence->waitCalls, 1);
    QCOMPARE(stalls, 2);
    QCOMPARE(queue.size(), 2);
}

QTEST_GUILESS_MAIN(TestEvictionGuard)
#include "tst_evictionguard.moc"
