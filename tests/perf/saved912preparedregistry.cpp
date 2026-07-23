#include "saved912preparedregistry.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <QMutex>
#include <QMutexLocker>

#include <array>
#include <atomic>
#include <memory_resource>
#include <optional>
#include <utility>

namespace {

constexpr size_t kPreparedSlotCount = 256;
constexpr qsizetype kInlineOwnerCount = 4;
enum class PreparedState : uint8_t { Free, Prepared, Signaled, Quarantined };
thread_local uint8_t currentAllocationPhase = 0;

class RetirementMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* value, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

RetirementMemoryResource& retirementMemoryResource() {
    static RetirementMemoryResource resource;
    return resource;
}

struct PreparedSlot {
    PreparedSlot() : overflowOwners(&retirementMemoryResource()) {}
    PreparedState state = PreparedState::Free;
    uint64_t reservation = 0;
    qsizetype ownerCount = 0;
    std::array<std::shared_ptr<GpuSurface>, kInlineOwnerCount> inlineOwners;
    std::pmr::vector<std::shared_ptr<GpuSurface>> overflowOwners;
    std::shared_ptr<GpuFence> fence;
    std::optional<GpuRetirementTicket> ticket;
};

struct PreparedMetrics {
    qsizetype pendingOwners = 0;
    qsizetype highWaterMark = 0;
    qsizetype quarantineOwners = 0;
    uint64_t signalFailureCount = 0;
};

QMutex& preparedMutex() {
    static QMutex mutex;
    return mutex;
}
std::array<PreparedSlot, kPreparedSlotCount>& preparedSlots() {
    static std::array<PreparedSlot, kPreparedSlotCount> entries;
    return entries;
}
PreparedMetrics& preparedMetrics() {
    static PreparedMetrics metrics;
    return metrics;
}
std::atomic<uint64_t>& nextReservation() {
    static std::atomic<uint64_t> reservation{1};
    return reservation;
}

void clearPreparedSlot(PreparedSlot& slot) noexcept {
    slot.ticket.reset();
    slot.fence.reset();
    for (auto& owner : slot.inlineOwners)
        owner.reset();
    slot.overflowOwners.clear();
    slot.ownerCount = 0;
    slot.state = PreparedState::Free;
}
bool slotMatches(const PreparedSlot& slot, uint64_t reservation) noexcept {
    return slot.state != PreparedState::Free && slot.reservation == reservation;
}

void drainPreparedCompleted() {
    struct CompletionProbe {
        size_t slot = 0;
        uint64_t reservation = 0;
        std::shared_ptr<GpuFence> fence;
        uint64_t value = 0;
    };
    std::array<CompletionProbe, kPreparedSlotCount> probes;
    size_t probeCount = 0;
    {
        QMutexLocker locker(&preparedMutex());
        const auto& entries = preparedSlots();
        for (size_t i = 0; i < entries.size(); ++i) {
            const PreparedSlot& slot = entries[i];
            if (slot.state != PreparedState::Signaled || !slot.fence || !slot.ticket) continue;
            probes[probeCount++] =
                CompletionProbe{i, slot.reservation, slot.fence, slot.ticket->value()};
        }
    }
    std::array<bool, kPreparedSlotCount> completed{};
    for (size_t i = 0; i < probeCount; ++i)
        completed[i] = probes[i].fence->completedValue() >= probes[i].value;
    QMutexLocker locker(&preparedMutex());
    auto& entries = preparedSlots();
    auto& metrics = preparedMetrics();
    for (size_t i = 0; i < probeCount; ++i) {
        if (!completed[i]) continue;
        PreparedSlot& slot = entries[probes[i].slot];
        if (!slotMatches(slot, probes[i].reservation) || slot.state != PreparedState::Signaled)
            continue;
        metrics.pendingOwners -= slot.ownerCount;
        clearPreparedSlot(slot);
    }
}

} // namespace

void Saved912PreparedRegistry::drainCompleted() const {
    drainPreparedCompleted();
}

Saved912PreparedRegistry::PreparedBatch
Saved912PreparedRegistry::prepareRetirement(const std::shared_ptr<GpuSurface>* surfaces,
                                            qsizetype count,
                                            const std::shared_ptr<GpuFence>& fence) const noexcept {
    if (!surfaces || count <= 0 || !fence) return {};
    QMutexLocker locker(&preparedMutex());
    auto& entries = preparedSlots();
    size_t slotIndex = entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].state == PreparedState::Free) {
            slotIndex = i;
            break;
        }
    }
    if (slotIndex == entries.size()) return {};
    PreparedSlot& slot = entries[slotIndex];
    qsizetype uniqueCount = 0;
    for (qsizetype i = 0; i < count; ++i) {
        if (!surfaces[i]) continue;
        bool duplicate = false;
        for (qsizetype j = 0; j < i; ++j) {
            if (surfaces[j].get() == surfaces[i].get()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) ++uniqueCount;
    }
    if (uniqueCount <= 0) return {};
    try {
        if (uniqueCount > kInlineOwnerCount)
            slot.overflowOwners.reserve(size_t(uniqueCount - kInlineOwnerCount));
        qsizetype inserted = 0;
        for (qsizetype i = 0; i < count; ++i) {
            if (!surfaces[i]) continue;
            bool duplicate = false;
            for (qsizetype j = 0; j < i; ++j) {
                if (surfaces[j].get() == surfaces[i].get()) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            if (inserted < kInlineOwnerCount)
                slot.inlineOwners[size_t(inserted)] = surfaces[i];
            else
                slot.overflowOwners.push_back(surfaces[i]);
            ++inserted;
        }
        slot.ownerCount = inserted;
        slot.fence = fence;
        slot.reservation = gpuSubmissionDetail::takeMonotonicInstanceId(nextReservation());
        if (slot.reservation == 0) {
            clearPreparedSlot(slot);
            return {};
        }
        slot.state = PreparedState::Prepared;
        return PreparedBatch(this, uint16_t(slotIndex), slot.reservation);
    } catch (...) {
        clearPreparedSlot(slot);
        return {};
    }
}

bool Saved912PreparedRegistry::publishPrepared(PreparedBatch& prepared,
                                               GpuRetirementTicket ticket) const noexcept {
    if (!prepared || prepared.m_registry != this) return false;
    QMutexLocker locker(&preparedMutex());
    PreparedSlot& slot = preparedSlots()[prepared.m_slot];
    if (!slotMatches(slot, prepared.m_reservation) || slot.state != PreparedState::Prepared)
        return false;
    slot.ticket.emplace(std::move(ticket));
    slot.state = PreparedState::Signaled;
    auto& metrics = preparedMetrics();
    metrics.pendingOwners += slot.ownerCount;
    metrics.highWaterMark = qMax(metrics.highWaterMark, metrics.pendingOwners);
    prepared.m_registry = nullptr;
    return true;
}

void Saved912PreparedRegistry::quarantinePrepared(PreparedBatch& prepared) const noexcept {
    if (!prepared || prepared.m_registry != this) return;
    QMutexLocker locker(&preparedMutex());
    PreparedSlot& slot = preparedSlots()[prepared.m_slot];
    if (slotMatches(slot, prepared.m_reservation) && slot.state == PreparedState::Prepared) {
        slot.state = PreparedState::Quarantined;
        auto& metrics = preparedMetrics();
        metrics.pendingOwners += slot.ownerCount;
        metrics.quarantineOwners += slot.ownerCount;
        metrics.highWaterMark = qMax(metrics.highWaterMark, metrics.pendingOwners);
    }
    prepared.m_registry = nullptr;
}

void Saved912PreparedRegistry::releasePrepared(PreparedBatch& prepared) const noexcept {
    if (!prepared || prepared.m_registry != this) return;
    QMutexLocker locker(&preparedMutex());
    PreparedSlot& slot = preparedSlots()[prepared.m_slot];
    if (slotMatches(slot, prepared.m_reservation) && slot.state == PreparedState::Prepared)
        clearPreparedSlot(slot);
    prepared.m_registry = nullptr;
}

Saved912PreparedRegistry::PreparedBatch::~PreparedBatch() {
    reset();
}
Saved912PreparedRegistry::PreparedBatch::PreparedBatch(PreparedBatch&& other) noexcept
    : m_registry(std::exchange(other.m_registry, nullptr)), m_slot(other.m_slot),
      m_reservation(other.m_reservation), m_accepted(other.m_accepted) {}
Saved912PreparedRegistry::PreparedBatch&
Saved912PreparedRegistry::PreparedBatch::operator=(PreparedBatch&& other) noexcept {
    if (this == &other) return *this;
    reset();
    m_registry = std::exchange(other.m_registry, nullptr);
    m_slot = other.m_slot;
    m_reservation = other.m_reservation;
    m_accepted = other.m_accepted;
    return *this;
}
void Saved912PreparedRegistry::PreparedBatch::reset() noexcept {
    if (!m_registry) return;
    if (m_accepted)
        m_registry->quarantinePrepared(*this);
    else
        m_registry->releasePrepared(*this);
}

Saved912PreparedRegistry::AllocationPhaseScope::AllocationPhaseScope(uint8_t phase) noexcept
    : m_previousPhase(Saved912PreparedRegistry::exchangeAllocationPhase(phase)) {}
Saved912PreparedRegistry::AllocationPhaseScope::AllocationPhaseScope(
    AllocationPhaseScope&& other) noexcept
    : m_previousPhase(other.m_previousPhase), m_active(std::exchange(other.m_active, false)) {}
Saved912PreparedRegistry::AllocationPhaseScope::~AllocationPhaseScope() {
    if (m_active) (void) Saved912PreparedRegistry::exchangeAllocationPhase(m_previousPhase);
}
void Saved912PreparedRegistry::AllocationPhaseScope::enterCallback() noexcept {
    (void) Saved912PreparedRegistry::exchangeAllocationPhase(2);
}
void Saved912PreparedRegistry::AllocationPhaseScope::enterPostAccept() noexcept {
    (void) Saved912PreparedRegistry::exchangeAllocationPhase(3);
}
Saved912PreparedRegistry::AllocationPhaseScope
Saved912PreparedRegistry::beginAllocationScope() const noexcept {
    return AllocationPhaseScope(1);
}
uint8_t Saved912PreparedRegistry::exchangeAllocationPhase(uint8_t phase) noexcept {
    return std::exchange(currentAllocationPhase, phase);
}
void Saved912PreparedRegistry::noteSignalFailure() const noexcept {
    QMutexLocker locker(&preparedMutex());
    ++preparedMetrics().signalFailureCount;
}
