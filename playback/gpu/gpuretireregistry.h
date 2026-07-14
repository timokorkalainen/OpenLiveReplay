#ifndef OLR_GPU_RETIRE_REGISTRY_H
#define OLR_GPU_RETIRE_REGISTRY_H

#include <QtGlobal>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

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

class GpuRetireRegistry final {
public:
    void drainCompleted() const;
    qsizetype pendingRetainCount() const;
    qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice) const;
    qsizetype abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) const;
    int drainWithBoundedWait(int totalTimeoutMs) const;
    GpuRetireDiagnostics diagnostics() const;

#ifdef OLR_UNIT_TEST
    static void failNextPreparationAllocationForTest() noexcept;
    static uint64_t preparationAllocationCountForTest() noexcept;
#endif

private:
    friend class GpuOpScope;
#ifdef OLR_UNIT_TEST
    friend struct GpuRetireRegistryTestAuthority;
#endif
    void registerRetire(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuFence> fence,
                        uint64_t fenceValue) const;
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
        PreparedBatch(const GpuRetireRegistry* registry, uint16_t slot,
                      uint64_t reservation) noexcept
            : m_registry(registry), m_slot(slot), m_reservation(reservation) {}
        void reset() noexcept;

        const GpuRetireRegistry* m_registry = nullptr;
        uint16_t m_slot = 0;
        uint64_t m_reservation = 0;
        bool m_accepted = false;
    };

    PreparedBatch prepareRetirement(const std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                                    const std::shared_ptr<GpuFence>& fence) const noexcept;
    bool publishPrepared(PreparedBatch& prepared, GpuRetirementTicket ticket) const noexcept;
    void quarantinePrepared(PreparedBatch& prepared) const noexcept;
    void releasePrepared(PreparedBatch& prepared) const noexcept;
    void noteSignalFailure() const;
};

#endif // OLR_GPU_RETIRE_REGISTRY_H
