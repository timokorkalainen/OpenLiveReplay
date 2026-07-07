// Multi-feed budget-pressure stress (spec Section 10 VRAM-blowup row): under a
// tiny GPU budget, concurrent gated mints must never push gatedLiveBytes past budgetBytes,
// over-budget mints must degrade to CPU handles, and charges must drain to zero
// once all handles drop.
#include <QtTest>

#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfaceallocator.h"
#include "playback/output/framehandle.h"

#include <atomic>
#include <array>
#include <thread>
#include <vector>

class TestGpuBudgetStress : public QObject {
    Q_OBJECT
private slots:
    void concurrentMintsNeverExceedBudgetAndNeverNull();
};

namespace {

class TestSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return GpuSurfaceDesc{FramePixelFormat::Nv12, 64, 48}; }
    bool isValid() const override { return true; }
    void* nativeHandle() const override { return nullptr; }
};

CpuPlanes solid(int w, int h) {
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = w;
    p.height = h;
    p.stride[0] = w;
    p.stride[1] = (w + 1) / 2;
    p.stride[2] = (w + 1) / 2;
    p.plane[0] = QByteArray(w * h, char(16));
    p.plane[1] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), char(128));
    p.plane[2] = QByteArray(((w + 1) / 2) * ((h + 1) / 2), char(128));
    return p;
}

FrameMetadata frameMeta(int feedIndex) {
    FrameMetadata meta;
    meta.key.feedIndex = feedIndex;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    return meta;
}

} // namespace

void TestGpuBudgetStress::concurrentMintsNeverExceedBudgetAndNeverNull() {
    constexpr int kFeedCount = 4;
    GpuBudgetConfig c;
    c.aggregateDecodeWindow = 4;
    c.feedCount = kFeedCount;
    c.stagingWindowPerFeed = 0;
    c.activeBusCount = 0;
    c.readbackRingDepth = 0;
    c.width = 64;
    c.height = 48;
    c.surfaceFormat = FramePixelFormat::Nv12;
    auto& b = GpuBudget::instance();
    b.reset();
    b.configure(c);

    std::atomic<bool> sawNull{false};
    std::atomic<bool> sawBlowup{false};
    std::atomic<int> gpuHandles{0};
    std::atomic<int> cpuDegrades{0};
    std::array<std::atomic<int>, kFeedCount> perFeedHandles;
    for (auto& count : perFeedHandles)
        count.store(0, std::memory_order_relaxed);
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&, t] {
            std::vector<FrameHandle> held;
            held.reserve(256);
            for (int i = 0; i < 256; ++i) {
                const int feedIndex = (t + i) % kFeedCount;
                auto surface = std::make_shared<TestSurface>();
                GpuMintResult r = mintGpuOrDegrade(
                    surface, frameMeta(feedIndex),
                    [](std::shared_ptr<GpuSurface> s, FrameMetadata m, GpuBudgetCharge charge) {
                        return makeGpuFrameHandle(std::move(s), nullptr, std::move(m), nullptr,
                                                  std::move(charge));
                    },
                    [] { return solid(64, 48); });

                if (r.handle.isNull()) sawNull.store(true, std::memory_order_release);
                if (r.handle.isGpuBacked()) gpuHandles.fetch_add(1, std::memory_order_acq_rel);
                if (r.degradedToCpu) cpuDegrades.fetch_add(1, std::memory_order_acq_rel);
                if (!r.handle.isNull())
                    perFeedHandles[feedIndex].fetch_add(1, std::memory_order_acq_rel);
                if (b.gatedLiveBytes() > b.budgetBytes())
                    sawBlowup.store(true, std::memory_order_release);
                held.push_back(std::move(r.handle));
            }
        });
    }
    for (auto& t : ts)
        t.join();

    QVERIFY(!sawNull.load(std::memory_order_acquire));
    QVERIFY(!sawBlowup.load(std::memory_order_acquire));
    QVERIFY(gpuHandles.load(std::memory_order_acquire) > 0);
    QVERIFY(cpuDegrades.load(std::memory_order_acquire) > 0);
    for (const auto& count : perFeedHandles)
        QVERIFY(count.load(std::memory_order_acquire) > 0);
    QCOMPARE(b.oomDegradeCount(), qint64(cpuDegrades.load(std::memory_order_acquire)));
    QCOMPARE(b.liveBytes(), qint64(0));
}

QTEST_GUILESS_MAIN(TestGpuBudgetStress)
#include "tst_gpu_budget_stress.moc"
