#include "playback/gpu/gpureadbackretainer.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

constexpr size_t kShardCount = 16;
constexpr size_t kNodesPerShard = 1024;
constexpr uint16_t kNoNode = std::numeric_limits<uint16_t>::max();
static_assert((kShardCount & (kShardCount - 1)) == 0,
              "retirement shard count must be a power of two");
static_assert(kNodesPerShard < size_t(kNoNode), "retirement node indices must fit uint16_t");
static_assert(std::is_nothrow_copy_constructible<GpuRetirementTicket>::value,
              "post-accept publication must not throw while copying exact tickets");

enum class RetireNodeState : uint8_t { Free, Prepared, Signaled, Quarantined };

struct RetireNode {
    RetireNodeState state = RetireNodeState::Free;
    uint64_t reservation = 0;
    GpuFenceIdentity identity;
    std::shared_ptr<GpuSurface> owner;
    std::shared_ptr<GpuFence> fence;
    std::optional<GpuRetirementTicket> ticket;
    uint16_t freeNext = kNoNode;
    uint16_t activeNext = kNoNode;
    uint16_t activePrevious = kNoNode;
    uint16_t batchNext = kNoNode;
};

struct RetireShard {
    QMutex mutex;
    std::array<RetireNode, kNodesPerShard> nodes;
    uint16_t freeHead = 0;
    uint16_t activeHead = kNoNode;
    size_t freeCount = kNodesPerShard;
};

struct RetireMetrics {
    std::atomic<qsizetype> pendingOwners{0};
    std::atomic<qsizetype> highWaterMark{0};
    std::atomic<qsizetype> quarantineOwners{0};
    std::atomic<uint64_t> timeoutCount{0};
    std::atomic<uint64_t> signalFailureCount{0};
};

#ifdef OLR_UNIT_TEST
struct StorageProbeMetrics {
    std::atomic<uint64_t> shardLockAcquisitions{0};
    std::atomic<uint64_t> activeNodesVisited{0};
    std::atomic<uint64_t> completionQueries{0};
    std::atomic<uint64_t> poolExhaustions{0};
    std::atomic<uint64_t> abandonmentShardVisits{0};
    std::atomic<uint64_t> abandonmentNodesVisited{0};
};
#endif

class RetireStorage final {
public:
    RetireStorage() {
        for (RetireShard& shard : shards) {
            for (size_t i = 0; i < kNodesPerShard; ++i)
                shard.nodes[i].freeNext = i + 1 < kNodesPerShard ? uint16_t(i + 1) : kNoNode;
        }
    }

    std::array<RetireShard, kShardCount> shards;
    RetireMetrics metrics;
#ifdef OLR_UNIT_TEST
    StorageProbeMetrics probe;
#endif
};

RetireStorage& storage() {
    static RetireStorage instance;
    return instance;
}

size_t shardIndexForDomain(uintptr_t deviceDomainId) noexcept {
    // Domain ownership makes authoritative abandonment a direct one-shard
    // operation. Exact fence instance/authority identity remains on every node.
    size_t folded = size_t(deviceDomainId);
    if ((folded & (kShardCount - 1)) == 0) {
        // Driver domains are commonly aligned native object addresses. Fold
        // higher address bits instead of collapsing every domain into shard 0.
        folded >>= 4;
        folded ^= folded >> 16;
        folded ^= folded >> 8;
    }
    return folded & (kShardCount - 1);
}

void noteShardLock() noexcept {
#ifdef OLR_UNIT_TEST
    storage().probe.shardLockAcquisitions.fetch_add(1, std::memory_order_relaxed);
#endif
}

void noteActiveVisit(uint64_t count = 1) noexcept {
#ifdef OLR_UNIT_TEST
    storage().probe.activeNodesVisited.fetch_add(count, std::memory_order_relaxed);
#else
    Q_UNUSED(count);
#endif
}

void updateHighWater(qsizetype value) noexcept {
    auto& highWater = storage().metrics.highWaterMark;
    qsizetype observed = highWater.load(std::memory_order_relaxed);
    while (observed < value &&
           !highWater.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
}

void linkActive(RetireShard& shard, uint16_t index) noexcept {
    RetireNode& node = shard.nodes[index];
    node.activePrevious = kNoNode;
    node.activeNext = shard.activeHead;
    if (shard.activeHead != kNoNode) shard.nodes[shard.activeHead].activePrevious = index;
    shard.activeHead = index;
}

void unlinkActive(RetireShard& shard, uint16_t index) noexcept {
    RetireNode& node = shard.nodes[index];
    if (node.activePrevious == kNoNode)
        shard.activeHead = node.activeNext;
    else
        shard.nodes[node.activePrevious].activeNext = node.activeNext;
    if (node.activeNext != kNoNode)
        shard.nodes[node.activeNext].activePrevious = node.activePrevious;
}

struct DeferredReleases {
    std::array<std::shared_ptr<GpuSurface>, kNodesPerShard> owners;
    std::array<std::shared_ptr<GpuFence>, kNodesPerShard> fences;
    size_t count = 0;
};

void recycleNode(RetireShard& shard, uint16_t index, DeferredReleases& releases) noexcept {
    RetireNode& node = shard.nodes[index];
    const RetireNodeState oldState = node.state;
    unlinkActive(shard, index);
    releases.owners[releases.count] = std::move(node.owner);
    releases.fences[releases.count] = std::move(node.fence);
    ++releases.count;
    node.ticket.reset();
    node.reservation = 0;
    node.identity = {};
    node.state = RetireNodeState::Free;
    node.activeNext = kNoNode;
    node.activePrevious = kNoNode;
    node.batchNext = kNoNode;
    node.freeNext = shard.freeHead;
    shard.freeHead = index;
    ++shard.freeCount;

    if (oldState == RetireNodeState::Signaled || oldState == RetireNodeState::Quarantined)
        storage().metrics.pendingOwners.fetch_sub(1, std::memory_order_relaxed);
    if (oldState == RetireNodeState::Quarantined)
        storage().metrics.quarantineOwners.fetch_sub(1, std::memory_order_relaxed);
}

bool batchMatches(const RetireShard& shard, const GpuRetirePreparedHandle& prepared,
                  RetireNodeState expected) noexcept {
    uint16_t index = prepared.head;
    for (uint16_t visited = 0; visited < prepared.count; ++visited) {
        if (index == kNoNode) return false;
        const RetireNode& node = shard.nodes[index];
        if (node.reservation != prepared.reservation || node.state != expected) return false;
        index = node.batchNext;
    }
    return index == kNoNode;
}

struct FenceProbe {
    GpuFenceIdentity identity;
    std::shared_ptr<GpuFence> fence;
    uint64_t maximumValue = 0;
    uint64_t completedValue = 0;
};

size_t findProbe(const std::array<FenceProbe, kNodesPerShard>& probes, size_t probeCount,
                 const GpuFenceIdentity& identity) noexcept {
    for (size_t i = 0; i < probeCount; ++i) {
        if (probes[i].identity == identity) return i;
    }
    return probeCount;
}

size_t collectFenceProbes(RetireShard& shard, std::array<FenceProbe, kNodesPerShard>& probes) {
    size_t probeCount = 0;
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    uint16_t index = shard.activeHead;
    while (index != kNoNode) {
        const RetireNode& node = shard.nodes[index];
        noteActiveVisit();
        if (node.state == RetireNodeState::Signaled && node.ticket && node.fence) {
            const size_t found = findProbe(probes, probeCount, node.identity);
            if (found == probeCount) {
                probes[probeCount].identity = node.identity;
                probes[probeCount].fence = node.fence;
                probes[probeCount].maximumValue = node.ticket->value();
                ++probeCount;
            } else {
                probes[found].maximumValue = qMax(probes[found].maximumValue, node.ticket->value());
            }
        }
        index = node.activeNext;
    }
    return probeCount;
}

void queryCompleted(std::array<FenceProbe, kNodesPerShard>& probes, size_t probeCount) {
    for (size_t i = 0; i < probeCount; ++i) {
#ifdef OLR_UNIT_TEST
        storage().probe.completionQueries.fetch_add(1, std::memory_order_relaxed);
#endif
        try {
            probes[i].completedValue = probes[i].fence->completedValue();
        } catch (...) {
            probes[i].completedValue = 0;
        }
    }
}

struct ReleaseCompletedResult {
    int released = 0;
    uint64_t timedOut = 0;
};

ReleaseCompletedResult releaseCompletedNodes(RetireShard& shard,
                                             const std::array<FenceProbe, kNodesPerShard>& probes,
                                             size_t probeCount, DeferredReleases& releases) {
    ReleaseCompletedResult result;
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    uint16_t index = shard.activeHead;
    while (index != kNoNode) {
        RetireNode& node = shard.nodes[index];
        const uint16_t next = node.activeNext;
        noteActiveVisit();
        if (node.state == RetireNodeState::Signaled && node.ticket) {
            const size_t found = findProbe(probes, probeCount, node.identity);
            if (found != probeCount && node.ticket->value() <= probes[found].completedValue) {
                recycleNode(shard, index, releases);
                ++result.released;
            } else {
                ++result.timedOut;
            }
        }
        index = next;
    }
    return result;
}

bool domainIsDead(uintptr_t domain, const std::vector<DeadDeviceToken>& deadDevices) noexcept {
    for (const DeadDeviceToken& token : deadDevices) {
        if (token.deviceDomainId() == domain) return true;
    }
    return false;
}

qsizetype abandonShardDomains(RetireShard& shard, uintptr_t singleDomain,
                              const std::vector<DeadDeviceToken>* deadDevices) {
    DeferredReleases releases;
    qsizetype released = 0;
    {
        QMutexLocker locker(&shard.mutex);
        noteShardLock();
#ifdef OLR_UNIT_TEST
        storage().probe.abandonmentShardVisits.fetch_add(1, std::memory_order_relaxed);
#endif
        uint16_t index = shard.activeHead;
        while (index != kNoNode) {
            RetireNode& node = shard.nodes[index];
            const uint16_t next = node.activeNext;
#ifdef OLR_UNIT_TEST
            storage().probe.abandonmentNodesVisited.fetch_add(1, std::memory_order_relaxed);
#endif
            const bool dead = deadDevices ? domainIsDead(node.identity.deviceDomainId, *deadDevices)
                                          : node.identity.deviceDomainId == singleDomain;
            if (dead) {
                recycleNode(shard, index, releases);
                ++released;
            }
            index = next;
        }
    }
    return released;
}

} // namespace

GpuRetirePreparedHandle GpuReadbackRetainer::prepare(const std::shared_ptr<GpuSurface>* surfaces,
                                                     qsizetype count,
                                                     const std::shared_ptr<GpuFence>& fence,
                                                     uint64_t reservation) noexcept {
    if (!surfaces || count <= 0 || !fence || reservation == 0 || count > qsizetype(kNodesPerShard))
        return {};

    qsizetype uniqueCount = 0;
    for (qsizetype i = 0; i < count; ++i) {
        if (!surfaces[i]) continue;
        bool duplicate = false;
        for (qsizetype previous = 0; previous < i; ++previous) {
            if (surfaces[previous].get() == surfaces[i].get()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) ++uniqueCount;
    }
    if (uniqueCount <= 0) return {};

    const GpuFenceIdentity identity = fence->identity();
    const size_t shardIndex = shardIndexForDomain(identity.deviceDomainId);
    RetireShard& shard = storage().shards[shardIndex];
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    if (shard.freeCount < size_t(uniqueCount)) {
#ifdef OLR_UNIT_TEST
        storage().probe.poolExhaustions.fetch_add(1, std::memory_order_relaxed);
#endif
        return {};
    }

    uint16_t batchHead = kNoNode;
    uint16_t inserted = 0;
    for (qsizetype i = 0; i < count; ++i) {
        if (!surfaces[i]) continue;
        bool duplicate = false;
        for (qsizetype previous = 0; previous < i; ++previous) {
            if (surfaces[previous].get() == surfaces[i].get()) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        const uint16_t index = shard.freeHead;
        RetireNode& node = shard.nodes[index];
        shard.freeHead = node.freeNext;
        --shard.freeCount;
        node.freeNext = kNoNode;
        node.state = RetireNodeState::Prepared;
        node.reservation = reservation;
        node.identity = identity;
        node.owner = surfaces[i];
        node.fence = fence;
        node.ticket.reset();
        node.batchNext = batchHead;
        batchHead = index;
        linkActive(shard, index);
        ++inserted;
    }
    return GpuRetirePreparedHandle{uint16_t(shardIndex), batchHead, inserted, reservation};
}

bool GpuReadbackRetainer::publish(const GpuRetirePreparedHandle& prepared,
                                  const GpuRetirementTicket& ticket) noexcept {
    if (!prepared || prepared.shard >= kShardCount || ticket.identity().deviceDomainId == 0)
        return false;
    RetireShard& shard = storage().shards[prepared.shard];
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    if (!batchMatches(shard, prepared, RetireNodeState::Prepared)) return false;

    uint16_t index = prepared.head;
    for (uint16_t visited = 0; visited < prepared.count; ++visited) {
        const RetireNode& node = shard.nodes[index];
        if (node.identity != ticket.identity()) return false;
        index = node.batchNext;
    }
    index = prepared.head;
    for (uint16_t visited = 0; visited < prepared.count; ++visited) {
        RetireNode& node = shard.nodes[index];
        node.ticket.emplace(ticket);
        node.state = RetireNodeState::Signaled;
        index = node.batchNext;
    }
    const qsizetype pending =
        storage().metrics.pendingOwners.fetch_add(prepared.count, std::memory_order_relaxed) +
        prepared.count;
    updateHighWater(pending);
    return true;
}

void GpuReadbackRetainer::quarantine(const GpuRetirePreparedHandle& prepared) noexcept {
    if (!prepared || prepared.shard >= kShardCount) return;
    RetireShard& shard = storage().shards[prepared.shard];
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    if (!batchMatches(shard, prepared, RetireNodeState::Prepared)) return;

    uint16_t index = prepared.head;
    for (uint16_t visited = 0; visited < prepared.count; ++visited) {
        RetireNode& node = shard.nodes[index];
        node.state = RetireNodeState::Quarantined;
        index = node.batchNext;
    }
    const qsizetype pending =
        storage().metrics.pendingOwners.fetch_add(prepared.count, std::memory_order_relaxed) +
        prepared.count;
    storage().metrics.quarantineOwners.fetch_add(prepared.count, std::memory_order_relaxed);
    updateHighWater(pending);
}

void GpuReadbackRetainer::release(const GpuRetirePreparedHandle& prepared) noexcept {
    if (!prepared || prepared.shard >= kShardCount) return;
    DeferredReleases releases;
    RetireShard& shard = storage().shards[prepared.shard];
    {
        QMutexLocker locker(&shard.mutex);
        noteShardLock();
        if (!batchMatches(shard, prepared, RetireNodeState::Prepared)) return;
        uint16_t index = prepared.head;
        for (uint16_t visited = 0; visited < prepared.count; ++visited) {
            const uint16_t next = shard.nodes[index].batchNext;
            recycleNode(shard, index, releases);
            index = next;
        }
    }
}

void GpuReadbackRetainer::drainCompleted() {
    for (RetireShard& shard : storage().shards) {
        std::array<FenceProbe, kNodesPerShard> probes;
        const size_t probeCount = collectFenceProbes(shard, probes);
        if (probeCount == 0) continue;
        queryCompleted(probes, probeCount);
        DeferredReleases releases;
        (void) releaseCompletedNodes(shard, probes, probeCount, releases);
    }
}

qsizetype GpuReadbackRetainer::pendingCount() noexcept {
    return storage().metrics.pendingOwners.load(std::memory_order_relaxed);
}

qsizetype GpuReadbackRetainer::abandonAllNoWait(const DeadDeviceToken& deadDevice) {
    const uintptr_t domain = deadDevice.deviceDomainId();
    if (domain == 0) return 0;
    return abandonShardDomains(storage().shards[shardIndexForDomain(domain)], domain, nullptr);
}

qsizetype GpuReadbackRetainer::abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) {
    if (deadDevices.empty()) return 0;
    std::array<bool, kShardCount> visit{};
    for (const DeadDeviceToken& token : deadDevices) {
        if (token.deviceDomainId() != 0) visit[shardIndexForDomain(token.deviceDomainId())] = true;
    }

    qsizetype released = 0;
    for (size_t shardIndex = 0; shardIndex < kShardCount; ++shardIndex) {
        if (visit[shardIndex])
            released += abandonShardDomains(storage().shards[shardIndex], 0, &deadDevices);
    }
    return released;
}

int GpuReadbackRetainer::drainWithBoundedWait(int totalTimeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    int released = 0;
    uint64_t timedOut = 0;
    for (RetireShard& shard : storage().shards) {
        std::array<FenceProbe, kNodesPerShard> probes;
        const size_t probeCount = collectFenceProbes(shard, probes);
        if (probeCount == 0) continue;
        queryCompleted(probes, probeCount);
        for (size_t i = 0; i < probeCount; ++i) {
            if (probes[i].completedValue >= probes[i].maximumValue) continue;
            const int remainingMs = qMax(0, totalTimeoutMs - int(elapsed.elapsed()));
            if (remainingMs <= 0) continue;
            try {
                if (probes[i].fence->wait(probes[i].maximumValue, remainingMs))
                    probes[i].completedValue = probes[i].maximumValue;
            } catch (...) {
            }
        }

        DeferredReleases releases;
        const ReleaseCompletedResult result =
            releaseCompletedNodes(shard, probes, probeCount, releases);
        timedOut += result.timedOut;
        released += result.released;
    }
    storage().metrics.timeoutCount.fetch_add(timedOut, std::memory_order_relaxed);
    return released;
}

qsizetype GpuReadbackRetainer::highWaterMark() noexcept {
    return storage().metrics.highWaterMark.load(std::memory_order_relaxed);
}

uint64_t GpuReadbackRetainer::timeoutCount() noexcept {
    return storage().metrics.timeoutCount.load(std::memory_order_relaxed);
}

uint64_t GpuReadbackRetainer::signalFailureCount() noexcept {
    return storage().metrics.signalFailureCount.load(std::memory_order_relaxed);
}

qsizetype GpuReadbackRetainer::quarantineCount() noexcept {
    return storage().metrics.quarantineOwners.load(std::memory_order_relaxed);
}

void GpuReadbackRetainer::noteSignalFailure() noexcept {
    storage().metrics.signalFailureCount.fetch_add(1, std::memory_order_relaxed);
}

#ifdef OLR_UNIT_TEST
void GpuReadbackRetainer::resetStorageProbeForTest() noexcept {
    auto& probe = storage().probe;
    probe.shardLockAcquisitions.store(0, std::memory_order_relaxed);
    probe.activeNodesVisited.store(0, std::memory_order_relaxed);
    probe.completionQueries.store(0, std::memory_order_relaxed);
    probe.poolExhaustions.store(0, std::memory_order_relaxed);
    probe.abandonmentShardVisits.store(0, std::memory_order_relaxed);
    probe.abandonmentNodesVisited.store(0, std::memory_order_relaxed);
}

GpuRetireStorageSnapshot GpuReadbackRetainer::storageSnapshotForTest() noexcept {
    const auto& probe = storage().probe;
    return GpuRetireStorageSnapshot{0,
                                    probe.shardLockAcquisitions.load(std::memory_order_relaxed),
                                    probe.activeNodesVisited.load(std::memory_order_relaxed),
                                    probe.completionQueries.load(std::memory_order_relaxed),
                                    probe.poolExhaustions.load(std::memory_order_relaxed),
                                    probe.abandonmentShardVisits.load(std::memory_order_relaxed),
                                    probe.abandonmentNodesVisited.load(std::memory_order_relaxed)};
}

size_t GpuReadbackRetainer::poolCapacityPerShardForTest() noexcept {
    return kNodesPerShard;
}

namespace gpuRetireDetail {
bool mutexAvailableForTest() {
    for (RetireShard& shard : storage().shards) {
        if (!shard.mutex.tryLock()) return false;
        shard.mutex.unlock();
    }
    return true;
}
} // namespace gpuRetireDetail
#endif
