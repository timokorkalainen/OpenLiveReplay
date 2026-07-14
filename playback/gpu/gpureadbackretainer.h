#ifndef OLR_GPU_READBACK_RETAINER_H
#define OLR_GPU_READBACK_RETAINER_H

#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class DeadDeviceToken;
class GpuFence;
class GpuRetireRegistry;
class GpuRetirementTicket;
class GpuSurface;

struct GpuRetirePreparedHandle {
    uint16_t shard = 0;
    uint16_t head = 0;
    uint16_t count = 0;
    uint64_t reservation = 0;

    explicit operator bool() const noexcept { return reservation != 0 && count != 0; }
};

struct GpuRetireMetricsSnapshot {
    qsizetype pendingOwners = 0;
    qsizetype highWaterMark = 0;
    qsizetype quarantineOwners = 0;
    uint64_t timeoutCount = 0;
    uint64_t signalFailureCount = 0;
};

#ifdef OLR_UNIT_TEST
using GpuRetireDiagnosticsHook = void (*)(void*) noexcept;

struct GpuRetireStorageSnapshot {
    uint64_t shardLockAcquisitions = 0;
    uint64_t drainShardVisits = 0;
    uint64_t activeNodesVisited = 0;
    uint64_t fenceGroupsVisited = 0;
    uint64_t fenceLookupSteps = 0;
    uint64_t completionQueries = 0;
    uint64_t poolNodeAcquisitions = 0;
    uint64_t poolNodeReleases = 0;
    uint64_t poolExhaustions = 0;
    uint64_t abandonmentShardVisits = 0;
    uint64_t abandonmentNodesVisited = 0;
};
#endif

// Internal fixed-capacity retirement storage. Publication capabilities remain
// private to GpuRetireRegistry; callers can neither construct records nor pair
// arbitrary surfaces with fences through this implementation type.
class GpuReadbackRetainer final {
private:
    friend class GpuRetireRegistry;

    static GpuRetirePreparedHandle prepare(const std::shared_ptr<GpuSurface>* surfaces,
                                           qsizetype count, const std::shared_ptr<GpuFence>& fence,
                                           uint64_t reservation) noexcept;
    static bool publish(const GpuRetirePreparedHandle& prepared,
                        const GpuRetirementTicket& ticket) noexcept;
    static void quarantine(const GpuRetirePreparedHandle& prepared) noexcept;
    static void release(const GpuRetirePreparedHandle& prepared) noexcept;
    static void drainCompleted();
    static qsizetype pendingCount() noexcept;
    static qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice);
    static qsizetype abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices);
    static int drainWithBoundedWait(int totalTimeoutMs);
    static GpuRetireMetricsSnapshot diagnosticsSnapshot() noexcept;
    static qsizetype highWaterMark() noexcept;
    static uint64_t timeoutCount() noexcept;
    static uint64_t signalFailureCount() noexcept;
    static qsizetype quarantineCount() noexcept;
    static void noteSignalFailure() noexcept;

#ifdef OLR_UNIT_TEST
    static void resetStorageProbeForTest() noexcept;
    static GpuRetireStorageSnapshot storageSnapshotForTest() noexcept;
    static size_t poolCapacityPerShardForTest() noexcept;
    static void setStorageProbeEnabledForTest(bool enabled) noexcept;
    static void setDiagnosticsHookForTest(GpuRetireDiagnosticsHook hook, void* context) noexcept;
#endif
};

#ifdef OLR_UNIT_TEST
namespace gpuRetireDetail {
bool mutexAvailableForTest();
} // namespace gpuRetireDetail
#endif

#endif // OLR_GPU_READBACK_RETAINER_H
