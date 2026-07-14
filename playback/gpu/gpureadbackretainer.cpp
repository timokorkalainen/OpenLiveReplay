#include "playback/gpu/gpureadbackretainer.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

constexpr size_t kShardCount = 16;
constexpr size_t kNodesPerShard = 1024;
constexpr size_t kFenceGroupsPerShard = kNodesPerShard;
constexpr size_t kFenceHashBucketCount = kFenceGroupsPerShard * 2;
constexpr uint16_t kNoNode = std::numeric_limits<uint16_t>::max();
constexpr uint16_t kNoFenceGroup = std::numeric_limits<uint16_t>::max();
static_assert((kShardCount & (kShardCount - 1)) == 0,
              "retirement shard count must be a power of two");
static_assert((kFenceHashBucketCount & (kFenceHashBucketCount - 1)) == 0,
              "fence hash bucket count must be a power of two");
static_assert(kNodesPerShard < size_t(kNoNode), "retirement node indices must fit uint16_t");
static_assert(kFenceGroupsPerShard < size_t(kNoFenceGroup),
              "fence group indices must fit uint16_t");
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
    uint16_t fenceGroup = kNoFenceGroup;
    uint16_t groupNext = kNoNode;
    uint16_t groupPrevious = kNoNode;
};

struct FenceGroup {
    bool inUse = false;
    GpuFenceIdentity identity;
    std::shared_ptr<GpuFence> fence;
    uint64_t serial = 0;
    uint64_t maximumValue = 0;
    uint16_t nodeCount = 0;
    uint16_t signaledCount = 0;
    uint16_t signaledHead = kNoNode;
    uint16_t hashNext = kNoFenceGroup;
    uint16_t activeNext = kNoFenceGroup;
    uint16_t activePrevious = kNoFenceGroup;
    uint16_t freeNext = kNoFenceGroup;
};

struct RetireShard {
    QMutex mutex;
    std::array<RetireNode, kNodesPerShard> nodes;
    std::array<FenceGroup, kFenceGroupsPerShard> fenceGroups;
    std::array<uint16_t, kFenceHashBucketCount> fenceHashHeads;
    uint16_t freeHead = 0;
    uint16_t activeHead = kNoNode;
    uint16_t fenceGroupFreeHead = 0;
    uint16_t activeFenceGroupHead = kNoFenceGroup;
    size_t freeCount = kNodesPerShard;
    qsizetype pendingOwners = 0;
    qsizetype quarantineOwners = 0;
};

struct RetireMetrics {
    std::atomic<qsizetype> pendingOwners{0};
    std::atomic<qsizetype> highWaterMark{0};
    std::atomic<uint64_t> timeoutCount{0};
    std::atomic<uint64_t> signalFailureCount{0};
};

#ifdef OLR_UNIT_TEST
struct StorageProbeMetrics {
    std::atomic<uint64_t> shardLockAcquisitions{0};
    std::atomic<uint64_t> drainShardVisits{0};
    std::atomic<uint64_t> activeNodesVisited{0};
    std::atomic<uint64_t> fenceGroupsVisited{0};
    std::atomic<uint64_t> fenceLookupSteps{0};
    std::atomic<uint64_t> completionQueries{0};
    std::atomic<uint64_t> poolNodeAcquisitions{0};
    std::atomic<uint64_t> poolNodeReleases{0};
    std::atomic<uint64_t> poolExhaustions{0};
    std::atomic<uint64_t> abandonmentShardVisits{0};
    std::atomic<uint64_t> abandonmentNodesVisited{0};
};
#endif

class RetireStorage final {
public:
    RetireStorage() {
        for (RetireShard& shard : shards) {
            shard.fenceHashHeads.fill(kNoFenceGroup);
            for (size_t i = 0; i < kNodesPerShard; ++i)
                shard.nodes[i].freeNext = i + 1 < kNodesPerShard ? uint16_t(i + 1) : kNoNode;
            for (size_t i = 0; i < kFenceGroupsPerShard; ++i) {
                shard.fenceGroups[i].freeNext =
                    i + 1 < kFenceGroupsPerShard ? uint16_t(i + 1) : kNoFenceGroup;
            }
        }
    }

    std::array<RetireShard, kShardCount> shards;
    std::atomic<uint32_t> activeShardMask{0};
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
    size_t folded = size_t(deviceDomainId);
    if ((folded & (kShardCount - 1)) == 0) {
        folded >>= 4;
        folded ^= folded >> 16;
        folded ^= folded >> 8;
    }
    return folded & (kShardCount - 1);
}

size_t fenceHash(const GpuFenceIdentity& identity) noexcept {
    uint64_t value = identity.instanceId * UINT64_C(0x9E3779B185EBCA87);
    value ^= uint64_t(identity.deviceDomainId) + UINT64_C(0x9E3779B97F4A7C15) + (value << 6) +
             (value >> 2);
    value ^= identity.authorityEpoch * UINT64_C(0xC2B2AE3D27D4EB4F);
    value ^= value >> 33;
    return size_t(value) & (kFenceHashBucketCount - 1);
}

void noteShardLock() noexcept {
#ifdef OLR_UNIT_TEST
    storage().probe.shardLockAcquisitions.fetch_add(1, std::memory_order_relaxed);
#endif
}

void noteActiveVisit() noexcept {
#ifdef OLR_UNIT_TEST
    storage().probe.activeNodesVisited.fetch_add(1, std::memory_order_relaxed);
#endif
}

void noteFenceGroupVisit() noexcept {
#ifdef OLR_UNIT_TEST
    storage().probe.fenceGroupsVisited.fetch_add(1, std::memory_order_relaxed);
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

uint16_t findFenceGroup(const RetireShard& shard, const GpuFenceIdentity& identity) noexcept {
    uint16_t index = shard.fenceHashHeads[fenceHash(identity)];
    while (index != kNoFenceGroup) {
#ifdef OLR_UNIT_TEST
        storage().probe.fenceLookupSteps.fetch_add(1, std::memory_order_relaxed);
#endif
        const FenceGroup& group = shard.fenceGroups[index];
        if (group.inUse && group.identity == identity) return index;
        index = group.hashNext;
    }
    return kNoFenceGroup;
}

uint16_t acquireFenceGroup(RetireShard& shard, const GpuFenceIdentity& identity,
                           const std::shared_ptr<GpuFence>& fence) noexcept {
    const uint16_t existing = findFenceGroup(shard, identity);
    if (existing != kNoFenceGroup) return existing;
    if (shard.fenceGroupFreeHead == kNoFenceGroup) return kNoFenceGroup;

    const uint16_t index = shard.fenceGroupFreeHead;
    FenceGroup& group = shard.fenceGroups[index];
    shard.fenceGroupFreeHead = group.freeNext;
    group.inUse = true;
    group.identity = identity;
    group.fence = fence;
    ++group.serial;
    if (group.serial == 0) ++group.serial;
    group.maximumValue = 0;
    group.nodeCount = 0;
    group.signaledCount = 0;
    group.signaledHead = kNoNode;
    group.activeNext = kNoFenceGroup;
    group.activePrevious = kNoFenceGroup;
    const size_t bucket = fenceHash(identity);
    group.hashNext = shard.fenceHashHeads[bucket];
    shard.fenceHashHeads[bucket] = index;
    group.freeNext = kNoFenceGroup;
    return index;
}

void activateFenceGroup(RetireShard& shard, size_t shardIndex, uint16_t index) noexcept {
    FenceGroup& group = shard.fenceGroups[index];
    if (group.activePrevious != kNoFenceGroup || group.activeNext != kNoFenceGroup ||
        shard.activeFenceGroupHead == index)
        return;
    group.activePrevious = kNoFenceGroup;
    group.activeNext = shard.activeFenceGroupHead;
    if (shard.activeFenceGroupHead != kNoFenceGroup)
        shard.fenceGroups[shard.activeFenceGroupHead].activePrevious = index;
    shard.activeFenceGroupHead = index;
    storage().activeShardMask.fetch_or(uint32_t(1u << shardIndex), std::memory_order_release);
}

void deactivateFenceGroup(RetireShard& shard, size_t shardIndex, uint16_t index) noexcept {
    FenceGroup& group = shard.fenceGroups[index];
    if (group.activePrevious == kNoFenceGroup)
        shard.activeFenceGroupHead = group.activeNext;
    else
        shard.fenceGroups[group.activePrevious].activeNext = group.activeNext;
    if (group.activeNext != kNoFenceGroup)
        shard.fenceGroups[group.activeNext].activePrevious = group.activePrevious;
    group.activeNext = kNoFenceGroup;
    group.activePrevious = kNoFenceGroup;
    if (shard.activeFenceGroupHead == kNoFenceGroup)
        storage().activeShardMask.fetch_and(~uint32_t(1u << shardIndex), std::memory_order_release);
}

void releaseFenceGroup(RetireShard& shard, uint16_t index) noexcept {
    FenceGroup& group = shard.fenceGroups[index];
    const size_t bucket = fenceHash(group.identity);
    uint16_t* link = &shard.fenceHashHeads[bucket];
    while (*link != kNoFenceGroup && *link != index)
        link = &shard.fenceGroups[*link].hashNext;
    if (*link == index) *link = group.hashNext;
    group.inUse = false;
    group.identity = {};
    group.fence.reset();
    group.maximumValue = 0;
    group.hashNext = kNoFenceGroup;
    group.freeNext = shard.fenceGroupFreeHead;
    shard.fenceGroupFreeHead = index;
}

void linkSignaledNode(RetireShard& shard, size_t shardIndex, uint16_t nodeIndex) noexcept {
    RetireNode& node = shard.nodes[nodeIndex];
    FenceGroup& group = shard.fenceGroups[node.fenceGroup];
    const bool wasEmpty = group.signaledCount == 0;
    node.groupPrevious = kNoNode;
    node.groupNext = group.signaledHead;
    if (group.signaledHead != kNoNode) shard.nodes[group.signaledHead].groupPrevious = nodeIndex;
    group.signaledHead = nodeIndex;
    ++group.signaledCount;
    if (wasEmpty) activateFenceGroup(shard, shardIndex, node.fenceGroup);
}

void detachNodeFromFenceGroup(RetireShard& shard, size_t shardIndex, uint16_t nodeIndex) noexcept {
    RetireNode& node = shard.nodes[nodeIndex];
    if (node.fenceGroup == kNoFenceGroup) return;
    const uint16_t groupIndex = node.fenceGroup;
    FenceGroup& group = shard.fenceGroups[groupIndex];
    if (node.state == RetireNodeState::Signaled) {
        if (node.groupPrevious == kNoNode)
            group.signaledHead = node.groupNext;
        else
            shard.nodes[node.groupPrevious].groupNext = node.groupNext;
        if (node.groupNext != kNoNode)
            shard.nodes[node.groupNext].groupPrevious = node.groupPrevious;
        --group.signaledCount;
        if (group.signaledCount == 0) deactivateFenceGroup(shard, shardIndex, groupIndex);
    }
    node.groupNext = kNoNode;
    node.groupPrevious = kNoNode;
    node.fenceGroup = kNoFenceGroup;
    --group.nodeCount;
    if (group.nodeCount == 0) releaseFenceGroup(shard, groupIndex);
}

struct DeferredReleases {
    std::array<std::shared_ptr<GpuSurface>, kNodesPerShard> owners;
    std::array<std::shared_ptr<GpuFence>, kNodesPerShard> fences;
    size_t count = 0;
};

void recycleNode(RetireShard& shard, size_t shardIndex, uint16_t index,
                 DeferredReleases& releases) noexcept {
    RetireNode& node = shard.nodes[index];
    const RetireNodeState oldState = node.state;
    detachNodeFromFenceGroup(shard, shardIndex, index);
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
#ifdef OLR_UNIT_TEST
    storage().probe.poolNodeReleases.fetch_add(1, std::memory_order_relaxed);
#endif

    if (oldState == RetireNodeState::Signaled || oldState == RetireNodeState::Quarantined) {
        --shard.pendingOwners;
        storage().metrics.pendingOwners.fetch_sub(1, std::memory_order_relaxed);
    }
    if (oldState == RetireNodeState::Quarantined) --shard.quarantineOwners;
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
    uint16_t groupIndex = kNoFenceGroup;
    uint64_t groupSerial = 0;
    GpuFenceIdentity identity;
    std::shared_ptr<GpuFence> fence;
    uint64_t maximumValue = 0;
    uint64_t completedValue = 0;
};

size_t collectFenceGroups(RetireShard& shard, size_t shardIndex,
                          std::array<FenceProbe, kFenceGroupsPerShard>& probes) {
    size_t probeCount = 0;
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    uint16_t index = shard.activeFenceGroupHead;
    while (index != kNoFenceGroup) {
        const FenceGroup& group = shard.fenceGroups[index];
        noteFenceGroupVisit();
        probes[probeCount++] =
            FenceProbe{index, group.serial, group.identity, group.fence, group.maximumValue, 0};
        index = group.activeNext;
    }
    if (probeCount == 0)
        storage().activeShardMask.fetch_and(~uint32_t(1u << shardIndex), std::memory_order_release);
    return probeCount;
}

void noteCompletionQuery() noexcept {
#ifdef OLR_UNIT_TEST
    storage().probe.completionQueries.fetch_add(1, std::memory_order_relaxed);
#endif
}

uint64_t pollCompleted(const std::shared_ptr<GpuFence>& fence) noexcept {
    noteCompletionQuery();
    try {
        return fence->completedValue();
    } catch (...) {
        return 0;
    }
}

void queryCompleted(std::array<FenceProbe, kFenceGroupsPerShard>& probes, size_t probeCount) {
    for (size_t i = 0; i < probeCount; ++i)
        probes[i].completedValue = pollCompleted(probes[i].fence);
}

struct ReleaseCompletedResult {
    int released = 0;
    uint64_t timedOut = 0;
};

ReleaseCompletedResult
releaseCompletedGroups(RetireShard& shard, size_t shardIndex,
                       const std::array<FenceProbe, kFenceGroupsPerShard>& probes,
                       size_t probeCount, DeferredReleases& releases) {
    ReleaseCompletedResult result;
    QMutexLocker locker(&shard.mutex);
    noteShardLock();
    for (size_t probeIndex = 0; probeIndex < probeCount; ++probeIndex) {
        const FenceProbe& probe = probes[probeIndex];
        if (probe.groupIndex >= kFenceGroupsPerShard) continue;
        FenceGroup& group = shard.fenceGroups[probe.groupIndex];
        if (!group.inUse || group.serial != probe.groupSerial || group.identity != probe.identity)
            continue;
        uint16_t nodeIndex = group.signaledHead;
        while (nodeIndex != kNoNode) {
            RetireNode& node = shard.nodes[nodeIndex];
            const uint16_t next = node.groupNext;
            noteActiveVisit();
            if (node.ticket && node.ticket->value() <= probe.completedValue) {
                recycleNode(shard, shardIndex, nodeIndex, releases);
                ++result.released;
            } else {
                ++result.timedOut;
            }
            nodeIndex = next;
        }
    }
    return result;
}

bool domainIsDead(uintptr_t domain, const std::vector<DeadDeviceToken>& deadDevices) noexcept {
    for (const DeadDeviceToken& token : deadDevices) {
        if (token.deviceDomainId() == domain) return true;
    }
    return false;
}

qsizetype abandonShardDomains(RetireShard& shard, size_t shardIndex, uintptr_t singleDomain,
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
                recycleNode(shard, shardIndex, index, releases);
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
    const uint16_t groupIndex = acquireFenceGroup(shard, identity, fence);
    if (groupIndex == kNoFenceGroup) {
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
        node.fenceGroup = groupIndex;
        node.groupNext = kNoNode;
        node.groupPrevious = kNoNode;
        batchHead = index;
        linkActive(shard, index);
        ++shard.fenceGroups[groupIndex].nodeCount;
#ifdef OLR_UNIT_TEST
        storage().probe.poolNodeAcquisitions.fetch_add(1, std::memory_order_relaxed);
#endif
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
        if (node.identity != ticket.identity() || node.fenceGroup == kNoFenceGroup) return false;
        index = node.batchNext;
    }
    index = prepared.head;
    for (uint16_t visited = 0; visited < prepared.count; ++visited) {
        RetireNode& node = shard.nodes[index];
        node.ticket.emplace(ticket);
        node.state = RetireNodeState::Signaled;
        FenceGroup& group = shard.fenceGroups[node.fenceGroup];
        group.maximumValue = std::max(group.maximumValue, ticket.value());
        linkSignaledNode(shard, prepared.shard, index);
        index = node.batchNext;
    }
    shard.pendingOwners += prepared.count;
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
        detachNodeFromFenceGroup(shard, prepared.shard, index);
        node.state = RetireNodeState::Quarantined;
        index = node.batchNext;
    }
    shard.pendingOwners += prepared.count;
    shard.quarantineOwners += prepared.count;
    const qsizetype pending =
        storage().metrics.pendingOwners.fetch_add(prepared.count, std::memory_order_relaxed) +
        prepared.count;
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
            recycleNode(shard, prepared.shard, index, releases);
            index = next;
        }
    }
}

void GpuReadbackRetainer::drainCompleted() {
    const uint32_t activeShards = storage().activeShardMask.load(std::memory_order_acquire);
    for (size_t shardIndex = 0; shardIndex < kShardCount; ++shardIndex) {
        if ((activeShards & uint32_t(1u << shardIndex)) == 0) continue;
#ifdef OLR_UNIT_TEST
        storage().probe.drainShardVisits.fetch_add(1, std::memory_order_relaxed);
#endif
        RetireShard& shard = storage().shards[shardIndex];
        std::array<FenceProbe, kFenceGroupsPerShard> probes;
        const size_t probeCount = collectFenceGroups(shard, shardIndex, probes);
        if (probeCount == 0) continue;
        queryCompleted(probes, probeCount);
        DeferredReleases releases;
        (void) releaseCompletedGroups(shard, shardIndex, probes, probeCount, releases);
    }
}

qsizetype GpuReadbackRetainer::pendingCount() noexcept {
    return storage().metrics.pendingOwners.load(std::memory_order_relaxed);
}

qsizetype GpuReadbackRetainer::abandonAllNoWait(const DeadDeviceToken& deadDevice) {
    const uintptr_t domain = deadDevice.deviceDomainId();
    if (domain == 0) return 0;
    const size_t shardIndex = shardIndexForDomain(domain);
    return abandonShardDomains(storage().shards[shardIndex], shardIndex, domain, nullptr);
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
            released +=
                abandonShardDomains(storage().shards[shardIndex], shardIndex, 0, &deadDevices);
    }
    return released;
}

int GpuReadbackRetainer::drainWithBoundedWait(int totalTimeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    int released = 0;
    uint64_t timedOut = 0;
    const uint32_t activeShards = storage().activeShardMask.load(std::memory_order_acquire);
    for (size_t shardIndex = 0; shardIndex < kShardCount; ++shardIndex) {
        if ((activeShards & uint32_t(1u << shardIndex)) == 0) continue;
#ifdef OLR_UNIT_TEST
        storage().probe.drainShardVisits.fetch_add(1, std::memory_order_relaxed);
#endif
        RetireShard& shard = storage().shards[shardIndex];
        std::array<FenceProbe, kFenceGroupsPerShard> probes;
        const size_t probeCount = collectFenceGroups(shard, shardIndex, probes);
        if (probeCount == 0) continue;
        queryCompleted(probes, probeCount);
        for (size_t i = 0; i < probeCount; ++i) {
            if (probes[i].completedValue >= probes[i].maximumValue) continue;
            const int remainingMs = qMax(0, totalTimeoutMs - int(elapsed.elapsed()));
            if (remainingMs <= 0) continue;
            bool completedMaximum = false;
            try {
                completedMaximum = probes[i].fence->wait(probes[i].maximumValue, remainingMs);
            } catch (...) {
            }
            if (completedMaximum)
                probes[i].completedValue = probes[i].maximumValue;
            else
                probes[i].completedValue = pollCompleted(probes[i].fence);
        }

        DeferredReleases releases;
        const ReleaseCompletedResult result =
            releaseCompletedGroups(shard, shardIndex, probes, probeCount, releases);
        timedOut += result.timedOut;
        released += result.released;
    }
    storage().metrics.timeoutCount.fetch_add(timedOut, std::memory_order_relaxed);
    return released;
}

GpuRetireMetricsSnapshot GpuReadbackRetainer::diagnosticsSnapshot() noexcept {
    GpuRetireMetricsSnapshot snapshot;
    for (RetireShard& shard : storage().shards) {
        QMutexLocker locker(&shard.mutex);
        noteShardLock();
        snapshot.pendingOwners += shard.pendingOwners;
        snapshot.quarantineOwners += shard.quarantineOwners;
    }
    snapshot.highWaterMark = std::max(
        snapshot.pendingOwners, storage().metrics.highWaterMark.load(std::memory_order_relaxed));
    snapshot.timeoutCount = storage().metrics.timeoutCount.load(std::memory_order_relaxed);
    snapshot.signalFailureCount =
        storage().metrics.signalFailureCount.load(std::memory_order_relaxed);
    return snapshot;
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
    return diagnosticsSnapshot().quarantineOwners;
}

void GpuReadbackRetainer::noteSignalFailure() noexcept {
    storage().metrics.signalFailureCount.fetch_add(1, std::memory_order_relaxed);
}

#ifdef OLR_UNIT_TEST
void GpuReadbackRetainer::resetStorageProbeForTest() noexcept {
    auto& probe = storage().probe;
    probe.shardLockAcquisitions.store(0, std::memory_order_relaxed);
    probe.drainShardVisits.store(0, std::memory_order_relaxed);
    probe.activeNodesVisited.store(0, std::memory_order_relaxed);
    probe.fenceGroupsVisited.store(0, std::memory_order_relaxed);
    probe.fenceLookupSteps.store(0, std::memory_order_relaxed);
    probe.completionQueries.store(0, std::memory_order_relaxed);
    probe.poolNodeAcquisitions.store(0, std::memory_order_relaxed);
    probe.poolNodeReleases.store(0, std::memory_order_relaxed);
    probe.poolExhaustions.store(0, std::memory_order_relaxed);
    probe.abandonmentShardVisits.store(0, std::memory_order_relaxed);
    probe.abandonmentNodesVisited.store(0, std::memory_order_relaxed);
}

GpuRetireStorageSnapshot GpuReadbackRetainer::storageSnapshotForTest() noexcept {
    const auto& probe = storage().probe;
    return GpuRetireStorageSnapshot{probe.shardLockAcquisitions.load(std::memory_order_relaxed),
                                    probe.drainShardVisits.load(std::memory_order_relaxed),
                                    probe.activeNodesVisited.load(std::memory_order_relaxed),
                                    probe.fenceGroupsVisited.load(std::memory_order_relaxed),
                                    probe.fenceLookupSteps.load(std::memory_order_relaxed),
                                    probe.completionQueries.load(std::memory_order_relaxed),
                                    probe.poolNodeAcquisitions.load(std::memory_order_relaxed),
                                    probe.poolNodeReleases.load(std::memory_order_relaxed),
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
