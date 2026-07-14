#ifndef OLR_GPU_RETIRE_REGISTRY_H
#define OLR_GPU_RETIRE_REGISTRY_H

#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpusubmission.h"

class DeadDeviceToken;
class GpuFence;
class GpuOpScope;
class GpuSurface;
#ifdef OLR_UNIT_TEST
struct GpuRetireRegistryTestAuthority;
#endif

struct GpuRetireDiagnostics {
    qsizetype pendingRetains = 0;
    qsizetype highWaterMark = 0;
    uint64_t timeoutCount = 0;
    uint64_t signalFailureCount = 0;
    qsizetype quarantineCount = 0;
};

#ifdef OLR_UNIT_TEST
struct GpuRetireAllocationSnapshot {
    uint64_t preparation = 0;
    uint64_t callback = 0;
    uint64_t postAccept = 0;
};
#endif

class GpuRetireRegistry final {
public:
    void drainCompleted() const;
    qsizetype pendingRetainCount() const;
    qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice) const;
    qsizetype abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) const;
    int drainWithBoundedWait(int totalTimeoutMs) const;
    GpuRetireDiagnostics diagnostics() const;

#ifdef OLR_UNIT_TEST
    static void failNextStorageAllocationForTest() noexcept;
    static void resetAllocationProbeForTest() noexcept;
    static GpuRetireAllocationSnapshot allocationSnapshotForTest() noexcept;
    static void resetStorageProbeForTest() noexcept;
    static GpuRetireStorageSnapshot storageSnapshotForTest() noexcept;
    static size_t poolCapacityPerShardForTest() noexcept;
#endif

private:
    friend class GpuOpScope;
    friend class GpuReadbackRetainer;
#ifdef OLR_UNIT_TEST
    friend struct GpuRetireRegistryTestAuthority;
    void registerRetire(std::shared_ptr<GpuSurface> surface, GpuRetirementTicket ticket) const;
#endif
    class AllocationPhaseScope final {
    public:
        AllocationPhaseScope(const AllocationPhaseScope&) = delete;
        AllocationPhaseScope& operator=(const AllocationPhaseScope&) = delete;
        AllocationPhaseScope(AllocationPhaseScope&& other) noexcept;
        ~AllocationPhaseScope();
        void enterCallback() noexcept;
        void enterPostAccept() noexcept;

    private:
        friend class GpuRetireRegistry;
        explicit AllocationPhaseScope(uint8_t initialPhase) noexcept;
        uint8_t m_previousPhase = 0;
        bool m_active = true;
    };
    class PreparedBatch final {
    public:
        PreparedBatch() = default;
        ~PreparedBatch();
        PreparedBatch(const PreparedBatch&) = delete;
        PreparedBatch& operator=(const PreparedBatch&) = delete;
        PreparedBatch(PreparedBatch&& other) noexcept;
        PreparedBatch& operator=(PreparedBatch&& other) noexcept;

        explicit operator bool() const noexcept { return m_registry != nullptr; }
        void markAccepted() noexcept { m_accepted = true; }
        bool accepted() const noexcept { return m_accepted; }

    private:
        friend class GpuRetireRegistry;
        PreparedBatch(const GpuRetireRegistry* registry, GpuRetirePreparedHandle handle) noexcept
            : m_registry(registry), m_handle(handle) {}
        void reset() noexcept;

        const GpuRetireRegistry* m_registry = nullptr;
        GpuRetirePreparedHandle m_handle;
        bool m_accepted = false;
    };

    PreparedBatch prepareRetirement(const std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                                    const std::shared_ptr<GpuFence>& fence) const noexcept;
    AllocationPhaseScope beginAllocationScope() const noexcept;
    static uint8_t exchangeAllocationPhase(uint8_t phase) noexcept;
    bool publishPrepared(PreparedBatch& prepared, GpuRetirementTicket ticket) const noexcept;
    void quarantinePrepared(PreparedBatch& prepared) const noexcept;
    void releasePrepared(PreparedBatch& prepared) const noexcept;
    void noteSignalFailure() const noexcept;
};

#endif // OLR_GPU_RETIRE_REGISTRY_H
