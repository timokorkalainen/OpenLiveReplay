#include <QtTest>

#include <QFile>
#include <QMutex>
#include <QMutexLocker>

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
#include <optional>
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

    template <typename SubmitFn>
    std::optional<GpuRetirementTicket>
    submitForBaseline(const GpuFenceIdentity& preparedFence,
                      const GpuSurfaceCompatibility& compatibility, uint64_t generation,
                      SubmitFn&& submitFn) {
        return submitExactForRetirement(preparedFence, compatibility, generation,
                                        std::forward<SubmitFn>(submitFn));
    }

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

GpuSubmissionResult submitFour(GpuRetireRegistry& registry, const std::shared_ptr<PerfFence>& fence,
                               const std::array<std::shared_ptr<GpuSurface>, 4>& surfaces) {
    PerfAdapter adapter;
    GpuOpScope operation(fence, registry);
    return operation.submit(adapter, GpuSurfacePack<4>(surfaces));
}

// Faithful saved model of the production GpuOpScope/prepared-slot path at
// 912663be: one mutex, 256 fixed slots, four inline owners, first-free linear
// preparation, a second publication lock, and one completion query per slot.
class Saved912663bePreparedRegistry final {
public:
    static constexpr size_t kPreparedSlotCount = 256;
    static constexpr size_t kInlineOwnerCount = 4;

    struct Operations {
        uint64_t registrationSlotVisits = 0;
        uint64_t drainSlotVisits = 0;
        uint64_t completionQueries = 0;
    };

    explicit Saved912663bePreparedRegistry(bool trackOperations = true)
        : mTrackOperations(trackOperations) {}

    Q_NEVER_INLINE bool submit(std::array<std::shared_ptr<GpuSurface>, kInlineOwnerCount> surfaces,
                               const std::shared_ptr<PerfFence>& fence) {
        if (!fence) return false;
        const uint64_t generation = GpuGenerationCounter::instance().current();
        const GpuFenceIdentity identity = fence->identity();
        std::array<GpuSurfaceCompatibility, kInlineOwnerCount> compatibilities{};
        GpuSurfaceCompatibility firstCompatibility{};
        for (size_t i = 0; i < surfaces.size(); ++i) {
            if (!surfaces[i]) return false;
            for (size_t previous = 0; previous < i; ++previous) {
                if (surfaces[previous].get() == surfaces[i].get()) return false;
            }
            const GpuSurfaceCompatibility compatibility = surfaces[i]->compatibility();
            if (!gpuSubmissionDetail::matchesSurfaceEvidence(
                    compatibility, identity, generation,
                    GpuGenerationCounter::instance().current()))
                return false;
            compatibilities[i] = compatibility;
            if (i == 0) firstCompatibility = compatibility;
        }

        size_t slotIndex = mSlots.size();
        uint64_t reservation = 0;
        {
            QMutexLocker locker(&mMutex);
            for (size_t i = 0; i < mSlots.size(); ++i) {
                if (mTrackOperations) ++mOperations.registrationSlotVisits;
                if (mSlots[i].state == State::Free) {
                    slotIndex = i;
                    break;
                }
            }
            if (slotIndex == mSlots.size()) return false;
            Slot& slot = mSlots[slotIndex];
            size_t uniqueCount = 0;
            for (size_t i = 0; i < surfaces.size(); ++i) {
                bool duplicate = false;
                for (size_t previous = 0; previous < i; ++previous) {
                    if (surfaces[previous].get() == surfaces[i].get()) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) ++uniqueCount;
            }
            if (uniqueCount == 0) return false;
            size_t inserted = 0;
            for (size_t i = 0; i < surfaces.size(); ++i) {
                bool duplicate = false;
                for (size_t previous = 0; previous < i; ++previous) {
                    if (surfaces[previous].get() == surfaces[i].get()) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) slot.owners[inserted++] = surfaces[i];
            }
            slot.ownerCount = inserted;
            slot.fence = fence;
            slot.reservation = reservation =
                gpuSubmissionDetail::takeMonotonicInstanceId(mNextReservation);
            if (reservation == 0) {
                clear(slot);
                return false;
            }
            slot.state = State::Prepared;
        }

        PerfAdapter adapter;
        auto ticket =
            fence->submitForBaseline(identity, firstCompatibility, generation, [&]() noexcept {
                return adapter() != GpuSubmitOutcome::NotSubmitted;
            });
        bool exact = ticket.has_value();
        if (ticket) {
            for (size_t i = 0; i < surfaces.size(); ++i) {
                if (!fence->validatesRetirement(*ticket, compatibilities[i])) {
                    exact = false;
                    break;
                }
            }
        }
        QMutexLocker locker(&mMutex);
        Slot& slot = mSlots[slotIndex];
        if (slot.state != State::Prepared || slot.reservation != reservation || !exact) {
            clear(slot);
            return false;
        }
        slot.ticket.emplace(std::move(*ticket));
        slot.state = State::Signaled;
        mPendingOwners += slot.ownerCount;
        mHighWaterMark = std::max(mHighWaterMark, mPendingOwners);
        return true;
    }

    Q_NEVER_INLINE void drainCompleted() {
        struct CompletionProbe {
            size_t slot = 0;
            uint64_t reservation = 0;
            std::shared_ptr<GpuFence> fence;
            uint64_t value = 0;
        };
        std::array<CompletionProbe, kPreparedSlotCount> probes;
        size_t probeCount = 0;
        {
            QMutexLocker locker(&mMutex);
            for (size_t i = 0; i < mSlots.size(); ++i) {
                if (mTrackOperations) ++mOperations.drainSlotVisits;
                const Slot& slot = mSlots[i];
                if (slot.state != State::Signaled || !slot.fence || !slot.ticket) continue;
                probes[probeCount++] =
                    CompletionProbe{i, slot.reservation, slot.fence, slot.ticket->value()};
            }
        }

        std::array<bool, kPreparedSlotCount> completed{};
        for (size_t i = 0; i < probeCount; ++i) {
            if (mTrackOperations) ++mOperations.completionQueries;
            completed[i] = probes[i].fence->completedValue() >= probes[i].value;
        }

        QMutexLocker locker(&mMutex);
        for (size_t i = 0; i < probeCount; ++i) {
            if (!completed[i]) continue;
            Slot& slot = mSlots[probes[i].slot];
            if (slot.state == State::Signaled && slot.reservation == probes[i].reservation) {
                mPendingOwners -= slot.ownerCount;
                clear(slot);
            }
        }
    }

    Operations operations() const noexcept { return mOperations; }

private:
    enum class State : uint8_t { Free, Prepared, Signaled };

    struct Slot {
        State state = State::Free;
        uint64_t reservation = 0;
        size_t ownerCount = 0;
        std::array<std::shared_ptr<GpuSurface>, kInlineOwnerCount> owners;
        std::shared_ptr<GpuFence> fence;
        std::optional<GpuRetirementTicket> ticket;
    };

    static void clear(Slot& slot) noexcept {
        for (auto& owner : slot.owners)
            owner.reset();
        slot.ticket.reset();
        slot.fence.reset();
        slot.ownerCount = 0;
        slot.state = State::Free;
    }

    QMutex mMutex;
    std::array<Slot, kPreparedSlotCount> mSlots;
    Operations mOperations;
    std::atomic<uint64_t> mNextReservation{1};
    size_t mPendingOwners = 0;
    size_t mHighWaterMark = 0;
    bool mTrackOperations = true;
};

class StorageProbeSuppression final {
public:
    StorageProbeSuppression() { GpuRetireRegistry::setStorageProbeEnabledForTest(false); }
    ~StorageProbeSuppression() { GpuRetireRegistry::setStorageProbeEnabledForTest(true); }
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
    constexpr uintptr_t deviceDomainId = 0x740;
    constexpr uint64_t authorityEpoch = 53;
    constexpr int operationCount = 256;
    constexpr int ownersPerOperation = 4;
    constexpr int sampleCount = 200;
    GpuGenerationCounter::instance().resetForTest();
    std::array<std::array<std::shared_ptr<GpuSurface>, ownersPerOperation>, operationCount>
        fixtures;
    for (int operation = 0; operation < operationCount; ++operation) {
        for (int owner = 0; owner < ownersPerOperation; ++owner) {
            fixtures[size_t(operation)][size_t(owner)] = std::make_shared<PerfSurface>(
                reinterpret_cast<void*>(
                    uintptr_t(0x10000 + operation * ownersPerOperation + owner)),
                GpuSurfaceCompatibility{deviceDomainId, authorityEpoch});
        }
    }

    std::vector<qint64> indexedRegistrationSamples;
    std::vector<qint64> indexedDrainSamples;
    indexedRegistrationSamples.reserve(sampleCount);
    indexedDrainSamples.reserve(sampleCount);

    {
        GpuRetireRegistry registry;
        auto fence = std::make_shared<PerfFence>(deviceDomainId, authorityEpoch);
        GpuRetireRegistry::resetStorageProbeForTest();
        for (const auto& owners : fixtures) {
            const auto result = submitFour(registry, fence, owners);
            QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
        }
        const GpuRetireStorageSnapshot registrationOperations =
            GpuRetireRegistry::storageSnapshotForTest();
        QCOMPARE(registrationOperations.poolNodeAcquisitions, uint64_t(operationCount));
        QCOMPARE(registrationOperations.poolExhaustions, uint64_t(0));
        fence->complete();
        GpuRetireRegistry::resetStorageProbeForTest();
        registry.drainCompleted();
        const GpuRetireStorageSnapshot drainOperations =
            GpuRetireRegistry::storageSnapshotForTest();
        QCOMPARE(drainOperations.drainShardVisits, uint64_t(1));
        QCOMPARE(drainOperations.fenceGroupsVisited, uint64_t(1));
        QCOMPARE(drainOperations.completionQueries, uint64_t(1));
        QCOMPARE(drainOperations.activeNodesVisited, uint64_t(operationCount));
        QCOMPARE(drainOperations.fenceLookupSteps, uint64_t(0));
        QCOMPARE(drainOperations.poolNodeReleases, uint64_t(operationCount));
    }

    for (int sample = 0; sample < sampleCount; ++sample) {
        GpuRetireRegistry registry;
        auto indexedFence = std::make_shared<PerfFence>(deviceDomainId, authorityEpoch);
        {
            StorageProbeSuppression suppressStorageProbe;
            const auto indexedRegistrationStarted = std::chrono::steady_clock::now();
            bool indexedPublishedAll = true;
            for (const auto& owners : fixtures) {
                const auto result = submitFour(registry, indexedFence, owners);
                indexedPublishedAll =
                    indexedPublishedAll && result.retirement == GpuRetirementDisposition::Published;
            }
            indexedRegistrationSamples.push_back(elapsedNanoseconds(indexedRegistrationStarted));
            QVERIFY(indexedPublishedAll);
            indexedFence->complete();
            const auto indexedDrainStarted = std::chrono::steady_clock::now();
            registry.drainCompleted();
            indexedDrainSamples.push_back(elapsedNanoseconds(indexedDrainStarted));
        }
    }

    {
        Saved912663bePreparedRegistry baseline;
        auto baselineFence = std::make_shared<PerfFence>(deviceDomainId, authorityEpoch);
        for (const auto& owners : fixtures)
            QVERIFY(baseline.submit(owners, baselineFence));
        const auto afterRegistration = baseline.operations();
        QCOMPARE(afterRegistration.registrationSlotVisits,
                 uint64_t(operationCount * (operationCount + 1) / 2));
        baselineFence->complete();
        baseline.drainCompleted();
        const auto afterDrain = baseline.operations();
        QCOMPARE(afterDrain.drainSlotVisits, uint64_t(operationCount));
        QCOMPARE(afterDrain.completionQueries, uint64_t(operationCount));
    }

    const qint64 indexedRegistrationMedian = percentile(indexedRegistrationSamples, 1, 2);
    const qint64 indexedRegistrationP95 = percentile(indexedRegistrationSamples, 95, 100);
    const qint64 indexedDrainMedian = percentile(indexedDrainSamples, 1, 2);
    const qint64 indexedDrainP95 = percentile(indexedDrainSamples, 95, 100);
    qInfo("Instrumented GPU retire unit path: registration=%lld/%lld ns; "
          "drain=%lld/%lld ns (median/p95); production wallclock gate runs separately",
          static_cast<long long>(indexedRegistrationMedian),
          static_cast<long long>(indexedRegistrationP95),
          static_cast<long long>(indexedDrainMedian), static_cast<long long>(indexedDrainP95));
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
    const QByteArray registryHeader =
        readSource(QStringLiteral("playback/gpu/gpuretireregistry.h"));
    const QByteArray retainerHeader =
        readSource(QStringLiteral("playback/gpu/gpureadbackretainer.h"));
    const QByteArray perf = readSource(QStringLiteral("tests/perf/tst_gpuretireregistry_perf.cpp"));
    const QByteArray savedOpScope = readSource(QStringLiteral("tests/perf/saved912gpuopscope.h"));
    const QByteArray savedRegistry =
        readSource(QStringLiteral("tests/perf/saved912preparedregistry.cpp"));
    const QByteArray savedRegistryHeader =
        readSource(QStringLiteral("tests/perf/saved912preparedregistry.h"));
    QVERIFY(!registry.isEmpty());
    QVERIFY(!retainer.isEmpty());
    QVERIFY(!registryHeader.isEmpty());
    QVERIFY(!retainerHeader.isEmpty());
    QVERIFY(!savedOpScope.isEmpty());
    QVERIFY(!savedRegistry.isEmpty());
    QVERIFY(!savedRegistryHeader.isEmpty());

    const QByteArray drain = functionBody(retainer, "void GpuReadbackRetainer::drainCompleted()");
    const QByteArray collect = functionBody(retainer, "size_t collectFenceGroups(");
    const QByteArray release = functionBody(retainer, "releaseCompletedGroups(");
    const QByteArray diagnostics =
        functionBody(registry, "GpuRetireDiagnostics GpuRetireRegistry::diagnostics()");
    QVERIFY(!drain.isEmpty());
    QVERIFY(!collect.isEmpty());
    QVERIFY(!release.isEmpty());
    QVERIFY(!diagnostics.isEmpty());
    const QByteArray savedBaselineDrain = functionBody(perf, "void drainCompleted()");
    QVERIFY(!savedBaselineDrain.isEmpty());
    QVERIFY2(!savedBaselineDrain.contains("QSet") && !savedBaselineDrain.contains("QVector"),
             "saved 912663be control must model its prepared-slot production path");
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
    const qsizetype retainerPrivate = retainerHeader.indexOf("private:");
    const qsizetype retainerPrepare =
        retainerHeader.indexOf("static GpuRetirePreparedHandle prepare(");
    QVERIFY2(retainerHeader.contains("friend class GpuRetireRegistry") && retainerPrivate >= 0 &&
                 retainerPrepare > retainerPrivate,
             "only the private registry capability may form fixed-pool prepared storage");
    const qsizetype registryPrivate = registryHeader.indexOf("private:");
    const qsizetype registryPrepare = registryHeader.indexOf("PreparedBatch prepareRetirement(");
    QVERIFY2(registryHeader.contains("friend class GpuOpScope") && registryPrivate >= 0 &&
                 registryPrepare > registryPrivate,
             "only private GpuOpScope authority may prepare or publish retirement batches");
    QVERIFY2(savedRegistryHeader.contains("912663be1458a0bdba36fca10c0421ab79d5cbd4") &&
                 savedRegistry.contains("constexpr size_t kPreparedSlotCount = 256") &&
                 savedRegistry.contains("std::array<PreparedSlot, kPreparedSlotCount>") &&
                 savedRegistry.contains("slot.ticket.emplace(std::move(ticket))") &&
                 savedOpScope.contains("m_registry.prepareRetirement") &&
                 savedOpScope.contains("m_registry.publishPrepared"),
             "portable production control must remain the frozen exact-912 prepared-slot path");
}

QTEST_GUILESS_MAIN(TestGpuRetireRegistryPerf)
#include "tst_gpuretireregistry_perf.moc"
