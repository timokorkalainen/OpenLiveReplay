#include <QtTest>

#include <QFile>
#include <QSet>
#include <QVector>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusubmission.h"
#include "playback/output/framepixelformat.h"

#include <algorithm>
#include <array>
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
    uint64_t signal() override { return ++mSignalled; }
    bool wait(uint64_t value, int) override { return mCompleted >= value; }
    uint64_t completedValue() const override { return mCompleted; }
    void complete() noexcept { mCompleted = mSignalled; }

private:
    uint64_t mSignalled = 0;
    uint64_t mCompleted = 0;
};

struct PerfAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

GpuSubmissionResult submitOne(GpuRetireRegistry& registry, const std::shared_ptr<PerfFence>& fence,
                              const std::shared_ptr<GpuSurface>& surface) {
    PerfAdapter adapter;
    GpuOpScope operation(fence, registry);
    return operation.submit(adapter,
                            GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
}

// Saved drain algorithm from 912663be. Fixture construction and registration
// happen outside the timed region; only the baseline copy/poll/QSet/remove drain
// is measured against the indexed implementation.
class Saved912663beRegistry final {
public:
    void registerRetire(const std::shared_ptr<GpuSurface>& surface,
                        const std::shared_ptr<GpuFence>& fence, uint64_t fenceValue) {
        mRetains.append(Retain{mNextId++, surface, fence, fenceValue});
    }

    void reserve(qsizetype count) { mRetains.reserve(count); }

    void drainCompleted() {
        const QVector<Retain> snapshot = mRetains;
        QSet<uint64_t> completedIds;
        completedIds.reserve(snapshot.size());
        for (const Retain& retain : snapshot) {
            if (retain.fence->completedValue() >= retain.fenceValue) completedIds.insert(retain.id);
        }
        if (completedIds.isEmpty()) return;
        for (qsizetype i = mRetains.size() - 1; i >= 0; --i) {
            if (completedIds.contains(mRetains.at(i).id)) mRetains.removeAt(i);
        }
    }

private:
    struct Retain {
        uint64_t id = 0;
        std::shared_ptr<GpuSurface> surface;
        std::shared_ptr<GpuFence> fence;
        uint64_t fenceValue = 0;
    };

    QVector<Retain> mRetains;
    uint64_t mNextId = 1;
};

qint64 elapsedNanoseconds(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                started)
        .count();
}

qint64 percentile(std::vector<qint64> samples, size_t numerator, size_t denominator) {
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() * numerator) / denominator];
}

bool commonPathAllocationGatePasses(const GpuRetireAllocationSnapshot& snapshot) noexcept {
    return snapshot.preparation == 0 && snapshot.callback == 0 && snapshot.postAccept == 0;
}

QByteArray readSource(const QString& relativePath) {
    QFile source(QStringLiteral(OLR_SOURCE_DIR "/") + relativePath);
    if (!source.open(QIODevice::ReadOnly)) return {};
    return source.readAll();
}

QByteArray functionBody(const QByteArray& source, const QByteArray& signature) {
    const qsizetype signatureOffset = source.indexOf(signature);
    if (signatureOffset < 0) return {};
    const qsizetype openBrace = source.indexOf('{', signatureOffset + signature.size());
    if (openBrace < 0) return {};
    int depth = 0;
    for (qsizetype i = openBrace; i < source.size(); ++i) {
        if (source.at(i) == '{')
            ++depth;
        else if (source.at(i) == '}' && --depth == 0)
            return source.mid(openBrace, i - openBrace + 1);
    }
    return {};
}

} // namespace

class TestGpuRetireRegistryPerf : public QObject {
    Q_OBJECT

private slots:
    void warmedCommonPathUsesPooledShards();
    void drainMeetsSaved912663beMedianAndP95();
    void allocationDetectorSeesInjectedHeapMutation();
    void implementationHasIndexedDrainAndCoherentDiagnostics();
};

void TestGpuRetireRegistryPerf::warmedCommonPathUsesPooledShards() {
    constexpr uintptr_t deviceDomainId = 0x701;
    constexpr uint64_t authorityEpoch = 51;
    constexpr int sampleCount = 512;
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<PerfFence>(deviceDomainId, authorityEpoch);
    std::vector<std::shared_ptr<GpuSurface>> fixtures;
    fixtures.reserve(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        fixtures.push_back(
            std::make_shared<PerfSurface>(reinterpret_cast<void*>(uintptr_t(0x8000 + sample)),
                                          GpuSurfaceCompatibility{deviceDomainId, authorityEpoch}));
    }

    (void) submitOne(registry, fence, fixtures.front());
    fence->complete();
    registry.drainCompleted();
    GpuRetireRegistry::resetStorageProbeForTest();
    GpuRetireRegistry::resetAllocationProbeForTest();

    std::vector<qint64> samples;
    samples.reserve(sampleCount);
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = submitOne(registry, fence, fixtures[size_t(sample)]);
        fence->complete();
        registry.drainCompleted();
        samples.push_back(elapsedNanoseconds(started));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    }

    const GpuRetireAllocationSnapshot allocations = GpuRetireRegistry::allocationSnapshotForTest();
    const GpuRetireStorageSnapshot storageSnapshot = GpuRetireRegistry::storageSnapshotForTest();
    QCOMPARE(allocations.preparation, uint64_t(0));
    QCOMPARE(allocations.callback, uint64_t(0));
    QCOMPARE(allocations.postAccept, uint64_t(0));
    QCOMPARE(storageSnapshot.poolExhaustions, uint64_t(0));
    QCOMPARE(storageSnapshot.poolNodeAcquisitions, uint64_t(sampleCount));
    QCOMPARE(storageSnapshot.poolNodeReleases, uint64_t(sampleCount));
    QCOMPARE(storageSnapshot.drainShardVisits, uint64_t(sampleCount));
    QCOMPARE(storageSnapshot.fenceGroupsVisited, uint64_t(sampleCount));
    QCOMPARE(storageSnapshot.activeNodesVisited, uint64_t(sampleCount));
    QCOMPARE(storageSnapshot.completionQueries, uint64_t(sampleCount));

    const qint64 median = percentile(samples, 1, 2);
    const qint64 p95 = percentile(samples, 95, 100);
    qInfo("GPU retire pooled operation: median=%lld ns p95=%lld ns pool=%llu/%llu "
          "shards=%llu groups=%llu queries=%llu nodes=%llu",
          static_cast<long long>(median), static_cast<long long>(p95),
          static_cast<unsigned long long>(storageSnapshot.poolNodeAcquisitions),
          static_cast<unsigned long long>(storageSnapshot.poolNodeReleases),
          static_cast<unsigned long long>(storageSnapshot.drainShardVisits),
          static_cast<unsigned long long>(storageSnapshot.fenceGroupsVisited),
          static_cast<unsigned long long>(storageSnapshot.completionQueries),
          static_cast<unsigned long long>(storageSnapshot.activeNodesVisited));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuRetireRegistryPerf::drainMeetsSaved912663beMedianAndP95() {
    constexpr uint64_t authorityEpoch = 53;
    constexpr int shardCount = 16;
    constexpr int recordsPerShard = 512;
    constexpr int totalRecords = shardCount * recordsPerShard;
    constexpr int sampleCount = 40;
    GpuGenerationCounter::instance().resetForTest();
    std::array<std::vector<std::shared_ptr<GpuSurface>>, shardCount> fixtures;
    std::array<std::shared_ptr<PerfFence>, shardCount> indexedFences;
    std::array<std::shared_ptr<PerfFence>, shardCount> baselineFences;
    for (int shard = 0; shard < shardCount; ++shard) {
        const uintptr_t domain = 0x700 + uintptr_t(shard);
        fixtures[size_t(shard)].reserve(recordsPerShard);
        indexedFences[size_t(shard)] = std::make_shared<PerfFence>(domain, authorityEpoch);
        baselineFences[size_t(shard)] =
            std::make_shared<PerfFence>(0x800 + uintptr_t(shard), authorityEpoch);
        for (int record = 0; record < recordsPerShard; ++record) {
            fixtures[size_t(shard)].push_back(std::make_shared<PerfSurface>(
                reinterpret_cast<void*>(uintptr_t(0x10000 + shard * recordsPerShard + record)),
                GpuSurfaceCompatibility{domain, authorityEpoch}));
        }
    }

    std::vector<qint64> indexedSamples;
    std::vector<qint64> baselineSamples;
    indexedSamples.reserve(sampleCount);
    baselineSamples.reserve(sampleCount);

    for (int sample = 0; sample < sampleCount; ++sample) {
        GpuRetireRegistry registry;
        for (int shard = 0; shard < shardCount; ++shard) {
            for (const auto& surface : fixtures[size_t(shard)]) {
                const auto result = submitOne(registry, indexedFences[size_t(shard)], surface);
                QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
            }
            indexedFences[size_t(shard)]->complete();
        }
        GpuRetireRegistry::resetStorageProbeForTest();
        const auto indexedStarted = std::chrono::steady_clock::now();
        registry.drainCompleted();
        indexedSamples.push_back(elapsedNanoseconds(indexedStarted));
        const GpuRetireStorageSnapshot operations = GpuRetireRegistry::storageSnapshotForTest();
        QCOMPARE(operations.drainShardVisits, uint64_t(shardCount));
        QCOMPARE(operations.fenceGroupsVisited, uint64_t(shardCount));
        QCOMPARE(operations.completionQueries, uint64_t(shardCount));
        QCOMPARE(operations.activeNodesVisited, uint64_t(totalRecords));
        QCOMPARE(operations.fenceLookupSteps, uint64_t(0));
        QCOMPARE(operations.poolNodeReleases, uint64_t(totalRecords));

        std::array<Saved912663beRegistry, shardCount> baselines;
        for (int shard = 0; shard < shardCount; ++shard) {
            baselines[size_t(shard)].reserve(recordsPerShard);
            for (const auto& surface : fixtures[size_t(shard)]) {
                const uint64_t value = baselineFences[size_t(shard)]->signal();
                baselines[size_t(shard)].registerRetire(surface, baselineFences[size_t(shard)],
                                                        value);
            }
            baselineFences[size_t(shard)]->complete();
        }
        const auto baselineStarted = std::chrono::steady_clock::now();
        for (Saved912663beRegistry& baseline : baselines)
            baseline.drainCompleted();
        baselineSamples.push_back(elapsedNanoseconds(baselineStarted));
    }

    const qint64 indexedMedian = percentile(indexedSamples, 1, 2);
    const qint64 indexedP95 = percentile(indexedSamples, 95, 100);
    const qint64 baselineMedian = percentile(baselineSamples, 1, 2);
    const qint64 baselineP95 = percentile(baselineSamples, 95, 100);
    qInfo("GPU retire drain vs saved 912663be: indexed median=%lld p95=%lld ns; "
          "baseline median=%lld p95=%lld ns",
          static_cast<long long>(indexedMedian), static_cast<long long>(indexedP95),
          static_cast<long long>(baselineMedian), static_cast<long long>(baselineP95));
    QVERIFY2(indexedMedian * 100 <= baselineMedian * 102,
             "indexed drain median regressed by more than 2% against saved 912663be");
    QVERIFY2(indexedP95 * 100 <= baselineP95 * 102,
             "indexed drain p95 regressed by more than 2% against saved 912663be");
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuRetireRegistryPerf::allocationDetectorSeesInjectedHeapMutation() {
    constexpr uintptr_t deviceDomainId = 0x721;
    constexpr uint64_t authorityEpoch = 59;
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<PerfFence>(deviceDomainId, authorityEpoch);
    auto surface = std::make_shared<PerfSurface>(
        reinterpret_cast<void*>(0xA000), GpuSurfaceCompatibility{deviceDomainId, authorityEpoch});
    GpuRetireRegistry::resetAllocationProbeForTest();
    GpuRetireRegistry::injectHeapAllocationForNextPhaseForTest(GpuRetireAllocationPhase::Callback);
    const auto result = submitOne(registry, fence, surface);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    const GpuRetireAllocationSnapshot mutation = GpuRetireRegistry::allocationSnapshotForTest();
    QCOMPARE(mutation.preparation, uint64_t(0));
    QCOMPARE(mutation.callback, uint64_t(1));
    QCOMPARE(mutation.postAccept, uint64_t(0));
    QVERIFY2(!commonPathAllocationGatePasses(mutation),
             "the real heap mutation must trip the zero-allocation performance gate");
    fence->complete();
    registry.drainCompleted();
    GpuRetireRegistry::resetAllocationProbeForTest();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuRetireRegistryPerf::implementationHasIndexedDrainAndCoherentDiagnostics() {
    const QByteArray registry = readSource(QStringLiteral("playback/gpu/gpuretireregistry.cpp"));
    const QByteArray retainer = readSource(QStringLiteral("playback/gpu/gpureadbackretainer.cpp"));
    QVERIFY(!registry.isEmpty());
    QVERIFY(!retainer.isEmpty());

    const QByteArray drain = functionBody(retainer, "void GpuReadbackRetainer::drainCompleted()");
    const QByteArray collect = functionBody(retainer, "size_t collectFenceGroups(");
    const QByteArray release = functionBody(retainer, "releaseCompletedGroups(");
    const QByteArray diagnostics =
        functionBody(registry, "GpuRetireDiagnostics GpuRetireRegistry::diagnostics()");
    QVERIFY(!drain.isEmpty());
    QVERIFY(!collect.isEmpty());
    QVERIFY(!release.isEmpty());
    QVERIFY(!diagnostics.isEmpty());
    QVERIFY2(drain.contains("activeShardMask"), "drain must skip inactive shards");
    QVERIFY2(drain.contains("collectFenceGroups"), "drain must start from active fence groups");
    QVERIFY2(collect.contains("activeFenceGroupHead"),
             "fence collection must traverse the intrusive active-group list");
    QVERIFY2(!collect.contains("activeHead"),
             "fence collection must not rediscover groups by walking every active node");
    QVERIFY2(release.contains("probe.groupIndex"),
             "release must address the collected fence group directly");
    QVERIFY2(release.contains("signaledHead"),
             "release must traverse only nodes in the matching fence group");
    QVERIFY2(!release.contains("findProbe"),
             "release must not linearly search all fence probes per node");
    QVERIFY2(diagnostics.contains("diagnosticsSnapshot"),
             "diagnostics must read one invariant-preserving storage snapshot");
    QVERIFY2(retainer.contains("struct FenceGroup") && retainer.contains("groupNext"),
             "retirement nodes must be structurally bucketed by exact fence identity");
}

QTEST_GUILESS_MAIN(TestGpuRetireRegistryPerf)
#include "tst_gpuretireregistry_perf.moc"
