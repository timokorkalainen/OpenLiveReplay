#include <QtTest>

#include <QFile>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusubmission.h"
#include "playback/output/framepixelformat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace {

class PerfSurface final : public GpuSurface {
public:
    PerfSurface(void* handle, GpuSurfaceCompatibility compatibility)
        : m_handle(handle), m_compatibility(compatibility) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16, 0}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override { return m_handle; }

private:
    void* m_handle = nullptr;
    GpuSurfaceCompatibility m_compatibility;
};

class PerfFence final : public GpuFence {
public:
    PerfFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t value, int) override { return m_completed >= value; }
    uint64_t completedValue() const override { return m_completed; }
    void complete() noexcept { m_completed = m_signalled; }

private:
    uint64_t m_signalled = 0;
    uint64_t m_completed = 0;
};

struct PerfAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

template <size_t N>
void runOperation(GpuRetireRegistry& registry, const std::shared_ptr<PerfFence>& fence,
                  uintptr_t deviceDomainId, uint64_t authorityEpoch, uintptr_t handleBase) {
    std::array<std::shared_ptr<GpuSurface>, N> surfaces;
    for (size_t i = 0; i < N; ++i)
        surfaces[i] =
            std::make_shared<PerfSurface>(reinterpret_cast<void*>(handleBase + i),
                                          GpuSurfaceCompatibility{deviceDomainId, authorityEpoch});
    PerfAdapter adapter;
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(adapter, GpuSurfacePack<N>(std::move(surfaces)));
    Q_ASSERT(result.retirement == GpuRetirementDisposition::Published);
    fence->complete();
    registry.drainCompleted();
}

QByteArray readSource(const QString& relativePath) {
    QFile source(QStringLiteral(OLR_SOURCE_DIR "/") + relativePath);
    if (!source.open(QIODevice::ReadOnly)) return {};
    return source.readAll();
}

} // namespace

class TestGpuRetireRegistryPerf : public QObject {
    Q_OBJECT

private slots:
    void warmedCommonPathUsesPooledShards();
    void implementationHasNoGlobalCopyScanRegistry();
};

void TestGpuRetireRegistryPerf::warmedCommonPathUsesPooledShards() {
    constexpr uintptr_t deviceDomainId = 0x701;
    constexpr uint64_t authorityEpoch = 51;
    constexpr int sampleCount = 512;
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<PerfFence>(deviceDomainId, authorityEpoch);

    runOperation<1>(registry, fence, deviceDomainId, authorityEpoch, 0x7100);
    runOperation<2>(registry, fence, deviceDomainId, authorityEpoch, 0x7200);
    runOperation<3>(registry, fence, deviceDomainId, authorityEpoch, 0x7300);
    runOperation<4>(registry, fence, deviceDomainId, authorityEpoch, 0x7400);
    GpuRetireRegistry::resetStorageProbeForTest();
    GpuRetireRegistry::resetAllocationProbeForTest();

    std::vector<qint64> samples;
    samples.reserve(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        switch (sample % 4) {
        case 0:
            runOperation<1>(registry, fence, deviceDomainId, authorityEpoch,
                            uintptr_t(0x8000 + sample * 8));
            break;
        case 1:
            runOperation<2>(registry, fence, deviceDomainId, authorityEpoch,
                            uintptr_t(0x8000 + sample * 8));
            break;
        case 2:
            runOperation<3>(registry, fence, deviceDomainId, authorityEpoch,
                            uintptr_t(0x8000 + sample * 8));
            break;
        default:
            runOperation<4>(registry, fence, deviceDomainId, authorityEpoch,
                            uintptr_t(0x8000 + sample * 8));
            break;
        }
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count());
    }

    const GpuRetireAllocationSnapshot allocations = GpuRetireRegistry::allocationSnapshotForTest();
    const GpuRetireStorageSnapshot storage = GpuRetireRegistry::storageSnapshotForTest();
    QCOMPARE(allocations.preparation, uint64_t(0));
    QCOMPARE(allocations.callback, uint64_t(0));
    QCOMPARE(allocations.postAccept, uint64_t(0));
    QCOMPARE(storage.storageAllocations, uint64_t(0));
    QCOMPARE(storage.poolExhaustions, uint64_t(0));
    QVERIFY(storage.shardLockAcquisitions > 0);
    QVERIFY(storage.completionQueries > 0);

    std::sort(samples.begin(), samples.end());
    const qint64 median = samples[samples.size() / 2];
    const qint64 p95 = samples[(samples.size() * 95) / 100];
    qInfo("GPU retire pooled common path: median=%lld ns p95=%lld ns shardLocks=%llu "
          "queries=%llu visited=%llu",
          static_cast<long long>(median), static_cast<long long>(p95),
          static_cast<unsigned long long>(storage.shardLockAcquisitions),
          static_cast<unsigned long long>(storage.completionQueries),
          static_cast<unsigned long long>(storage.activeNodesVisited));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuRetireRegistryPerf::implementationHasNoGlobalCopyScanRegistry() {
    const QByteArray registry = readSource(QStringLiteral("playback/gpu/gpuretireregistry.cpp"));
    const QByteArray retainer = readSource(QStringLiteral("playback/gpu/gpureadbackretainer.cpp"));
    QVERIFY(!registry.isEmpty());
    QVERIFY(!retainer.isEmpty());
    const QByteArray implementation = registry + retainer;
    QVERIFY2(!implementation.contains("QVector"), "retirement storage must not use QVector");
    QVERIFY2(!implementation.contains("QSet"), "retirement drain must not build a QSet");
    QVERIFY2(!implementation.contains("preparedMutex"),
             "prepared publication must not use a process-wide mutex");
    QVERIFY2(!implementation.contains("readbackRetainMutex"),
             "legacy registration must not retain a process-wide mutex");
    QVERIFY2(!implementation.contains("preparedSlots"),
             "drain must not scan the whole prepared-slot registry");
}

QTEST_GUILESS_MAIN(TestGpuRetireRegistryPerf)
#include "tst_gpuretireregistry_perf.moc"
