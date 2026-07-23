#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframeretirequeue.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

class FakeGpuSurface final : public GpuSurface {
public:
    explicit FakeGpuSurface(uint64_t pendingFence) { retainUntilFenceRetired(pendingFence); }

    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 4, 4}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
};

class FakeGpuFrameData final : public IFrameData {
public:
    FakeGpuFrameData(std::shared_ptr<FakeGpuSurface> surface, std::shared_ptr<GpuFence> fence,
                     uint64_t value)
        : m_surface(std::move(surface)), m_synchronization{std::move(fence), value, true} {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override { return {}; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    GpuFrameSynchronization gpuSynchronization() const override { return m_synchronization; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<FakeGpuSurface> m_surface;
    GpuFrameSynchronization m_synchronization;
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

class ThrowingFence final : public GpuFence {
public:
    enum class ThrowPoint { CompletedValue, Wait };

    explicit ThrowingFence(ThrowPoint throwPoint) : m_throwPoint(throwPoint) {}

    uint64_t signal() override { return 1; }
    bool wait(uint64_t, int) override {
        if (m_completed.load(std::memory_order_acquire)) return true;
        if (m_throwPoint == ThrowPoint::Wait) throw std::runtime_error("wait failed");
        return false;
    }
    uint64_t completedValue() const override {
        if (m_completed.load(std::memory_order_acquire)) return 1;
        if (m_throwPoint == ThrowPoint::CompletedValue)
            throw std::runtime_error("completedValue failed");
        return 0;
    }
    void complete() { m_completed.store(true, std::memory_order_release); }

private:
    ThrowPoint m_throwPoint;
    std::atomic<bool> m_completed{false};
};

static FrameHandle makeGpuFrame(uint64_t pendingFence, const std::shared_ptr<GpuFence>& fence) {
    auto surface = std::make_shared<FakeGpuSurface>(pendingFence);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = qint64(pendingFence);
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    return FrameHandle(std::make_shared<FakeGpuFrameData>(std::move(surface), fence, pendingFence),
                       meta);
}

class TestEvictionGuard : public QObject {
    Q_OBJECT
private slots:
    void ignoresCpuAndUnfencedGpuFrames();
    void timeoutRetainsFrameAndCountsStall();
    void completedFenceReleasesFrame();
    void drainBudgetLimitsFenceWaitsPerPass();
    void fenceExceptionsRemainUnretired();
};

void TestEvictionGuard::ignoresCpuAndUnfencedGpuFrames() {
    GpuFrameRetireQueue queue;
    auto fence = std::make_shared<FakeFence>();

    queue.collect(solidYuv420pHandle(4, 4, 16, 128, 128));
    queue.collect(makeGpuFrame(0, fence));

    QCOMPARE(queue.size(), 0);
}

void TestEvictionGuard::timeoutRetainsFrameAndCountsStall() {
    auto fence = std::make_shared<FakeFence>();
    auto data = std::static_pointer_cast<const IFrameData>(
        std::make_shared<FakeGpuFrameData>(std::make_shared<FakeGpuSurface>(7), fence, 7));
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 7;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    std::weak_ptr<const IFrameData> weakData = data;

    GpuFrameRetireQueue queue;
    queue.collect(FrameHandle(std::move(data), meta));

    int stalls = 0;
    QCOMPARE(queue.drain(0, &stalls), 0);

    QCOMPARE(stalls, 1);
    QCOMPARE(fence->waitCalls, 1);
    QCOMPARE(queue.size(), 1);
    QVERIFY(!weakData.expired());
}

void TestEvictionGuard::completedFenceReleasesFrame() {
    auto fence = std::make_shared<FakeFence>();
    auto data = std::static_pointer_cast<const IFrameData>(
        std::make_shared<FakeGpuFrameData>(std::make_shared<FakeGpuSurface>(4), fence, 4));
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = 4;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 4;
    meta.key.height = 4;
    std::weak_ptr<const IFrameData> weakData = data;

    GpuFrameRetireQueue queue;
    queue.collect(FrameHandle(std::move(data), meta));

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
    queue.collect(makeGpuFrame(4, fence));
    queue.collect(makeGpuFrame(5, fence));

    int stalls = 0;
    QCOMPARE(queue.drain(1, &stalls, 1), 0);

    QCOMPARE(fence->waitCalls, 1);
    QCOMPARE(stalls, 2);
    QCOMPARE(queue.size(), 2);
}

void TestEvictionGuard::fenceExceptionsRemainUnretired() {
    GpuFrameRetireQueue queue;
    auto completedValueFence =
        std::make_shared<ThrowingFence>(ThrowingFence::ThrowPoint::CompletedValue);
    auto waitFence = std::make_shared<ThrowingFence>(ThrowingFence::ThrowPoint::Wait);
    queue.collect(makeGpuFrame(4, completedValueFence));
    queue.collect(makeGpuFrame(5, waitFence));

    int stalls = 0;
    bool threw = false;
    int released = -1;
    try {
        released = queue.drain(0, &stalls);
    } catch (...) {
        threw = true;
    }

    QVERIFY(!threw);
    QCOMPARE(released, 0);
    QCOMPARE(stalls, 2);
    QCOMPARE(queue.size(), 2);
}

QTEST_GUILESS_MAIN(TestEvictionGuard)
#include "tst_evictionguard.moc"
