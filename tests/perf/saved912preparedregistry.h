#ifndef OLR_TESTS_PERF_SAVED912_PREPARED_REGISTRY_H
#define OLR_TESTS_PERF_SAVED912_PREPARED_REGISTRY_H

// Frozen production-path control copied from exact commit
// 912663be1458a0bdba36fca10c0421ab79d5cbd4. Keep this type isolated from the
// current registry: the relative benchmark must compile both with identical flags.

#include "playback/gpu/gpusubmission.h"

#include <QtGlobal>

#include <cstdint>
#include <memory>

class GpuFence;
class GpuSurface;
template <typename Fence>
class Saved912GpuOpScope;

class Saved912PreparedRegistry final {
public:
    void drainCompleted() const;

private:
    template <typename Fence>
    friend class Saved912GpuOpScope;

    class AllocationPhaseScope final {
    public:
        AllocationPhaseScope(const AllocationPhaseScope&) = delete;
        AllocationPhaseScope& operator=(const AllocationPhaseScope&) = delete;
        AllocationPhaseScope(AllocationPhaseScope&& other) noexcept;
        ~AllocationPhaseScope();
        void enterCallback() noexcept;
        void enterPostAccept() noexcept;

    private:
        friend class Saved912PreparedRegistry;
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
        friend class Saved912PreparedRegistry;
        PreparedBatch(const Saved912PreparedRegistry* registry, uint16_t slot,
                      uint64_t reservation) noexcept
            : m_registry(registry), m_slot(slot), m_reservation(reservation) {}
        void reset() noexcept;

        const Saved912PreparedRegistry* m_registry = nullptr;
        uint16_t m_slot = 0;
        uint64_t m_reservation = 0;
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

#endif
