#include "playback/gpu/gpuretireregistry.h"

#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <QMutex>
#include <QMutexLocker>

#include <array>
#include <atomic>
#include <optional>
#include <utility>

namespace {

constexpr size_t kPreparedSlotCount = 256;
constexpr qsizetype kInlineOwnerCount = 4;

enum class PreparedState : uint8_t { Free, Prepared, Signaled, Quarantined };

struct PreparedSlot {
    PreparedState state = PreparedState::Free;
    uint64_t reservation = 0;
    qsizetype ownerCount = 0;
    std::array<std::shared_ptr<GpuSurface>, kInlineOwnerCount> inlineOwners;
    std::vector<std::shared_ptr<GpuSurface>> overflowOwners;
    std::shared_ptr<GpuFence> fence;
    std::optional<GpuRetirementTicket> ticket;
};

struct PreparedMetrics {
    qsizetype pendingOwners = 0;
    qsizetype highWaterMark = 0;
    qsizetype quarantineOwners = 0;
    uint64_t timeoutCount = 0;
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

std::atomic<uint64_t>& preparationAllocations() {
    static std::atomic<uint64_t> count{0};
    return count;
}

#ifdef OLR_UNIT_TEST
std::atomic<bool>& failNextPreparationAllocation() {
    static std::atomic<bool> fail{false};
    return fail;
}
#endif

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

qsizetype preparedPendingCount() {
    QMutexLocker locker(&preparedMutex());
    return preparedMetrics().pendingOwners;
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
        auto& entries = preparedSlots();
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

qsizetype abandonPreparedDomains(const std::vector<DeadDeviceToken>& deadDevices) {
    if (deadDevices.empty()) return 0;
    QMutexLocker locker(&preparedMutex());
    auto& entries = preparedSlots();
    auto& metrics = preparedMetrics();
    qsizetype released = 0;
    for (PreparedSlot& slot : entries) {
        if (slot.state == PreparedState::Free || !slot.fence) continue;
        bool dead = false;
        for (const DeadDeviceToken& token : deadDevices) {
            if (token.deviceDomainId() == slot.fence->deviceDomainId()) {
                dead = true;
                break;
            }
        }
        if (!dead) continue;
        released += slot.ownerCount;
        if (slot.state != PreparedState::Prepared) {
            metrics.pendingOwners -= slot.ownerCount;
            if (slot.state == PreparedState::Quarantined)
                metrics.quarantineOwners -= slot.ownerCount;
        }
        clearPreparedSlot(slot);
    }
    return released;
}

} // namespace

void GpuRetireRegistry::registerRetire(std::shared_ptr<GpuSurface> surface,
                                       std::shared_ptr<GpuFence> fence, uint64_t fenceValue) const {
    gpuRetireDetail::registerRetire(std::move(surface), std::move(fence), fenceValue);
}

void GpuRetireRegistry::drainCompleted() const {
    drainPreparedCompleted();
    gpuRetireDetail::drainCompleted();
}

qsizetype GpuRetireRegistry::pendingRetainCount() const {
    drainPreparedCompleted();
    return preparedPendingCount() + gpuRetireDetail::pendingCount();
}

qsizetype GpuRetireRegistry::abandonAllNoWait(const DeadDeviceToken& deadDevice) const {
    return abandonPreparedDomains(std::vector<DeadDeviceToken>{deadDevice}) +
           gpuRetireDetail::abandonAllNoWait(deadDevice);
}

qsizetype
GpuRetireRegistry::abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) const {
    return abandonPreparedDomains(deadDevices) + gpuRetireDetail::abandonAllNoWait(deadDevices);
}

int GpuRetireRegistry::drainWithBoundedWait(int totalTimeoutMs) const {
    return gpuRetireDetail::drainWithBoundedWait(totalTimeoutMs);
}

GpuRetireDiagnostics GpuRetireRegistry::diagnostics() const {
    drainPreparedCompleted();
    const qsizetype legacyPending = gpuRetireDetail::pendingCount();
    const qsizetype legacyHighWater = gpuRetireDetail::highWaterMark();
    const uint64_t legacyTimeouts = gpuRetireDetail::timeoutCount();
    const uint64_t legacySignalFailures = gpuRetireDetail::signalFailureCount();
    QMutexLocker locker(&preparedMutex());
    const PreparedMetrics& metrics = preparedMetrics();
    return GpuRetireDiagnostics{
        legacyPending + metrics.pendingOwners, qMax(legacyHighWater, metrics.highWaterMark),
        legacyTimeouts + metrics.timeoutCount, legacySignalFailures + metrics.signalFailureCount,
        metrics.quarantineOwners};
}

GpuRetireRegistry::PreparedBatch
GpuRetireRegistry::prepareRetirement(const std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
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
        if (uniqueCount > kInlineOwnerCount) {
            preparationAllocations().fetch_add(1, std::memory_order_relaxed);
#ifdef OLR_UNIT_TEST
            if (failNextPreparationAllocation().exchange(false, std::memory_order_relaxed))
                return {};
#endif
            slot.overflowOwners.reserve(size_t(uniqueCount - kInlineOwnerCount));
        }

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

bool GpuRetireRegistry::publishPrepared(PreparedBatch& prepared,
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

void GpuRetireRegistry::quarantinePrepared(PreparedBatch& prepared) const noexcept {
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

void GpuRetireRegistry::releasePrepared(PreparedBatch& prepared) const noexcept {
    if (!prepared || prepared.m_registry != this) return;
    QMutexLocker locker(&preparedMutex());
    PreparedSlot& slot = preparedSlots()[prepared.m_slot];
    if (slotMatches(slot, prepared.m_reservation) && slot.state == PreparedState::Prepared)
        clearPreparedSlot(slot);
    prepared.m_registry = nullptr;
}

GpuRetireRegistry::PreparedBatch::~PreparedBatch() {
    reset();
}

GpuRetireRegistry::PreparedBatch::PreparedBatch(PreparedBatch&& other) noexcept
    : m_registry(std::exchange(other.m_registry, nullptr)), m_slot(other.m_slot),
      m_reservation(other.m_reservation), m_accepted(other.m_accepted) {}

GpuRetireRegistry::PreparedBatch&
GpuRetireRegistry::PreparedBatch::operator=(PreparedBatch&& other) noexcept {
    if (this == &other) return *this;
    reset();
    m_registry = std::exchange(other.m_registry, nullptr);
    m_slot = other.m_slot;
    m_reservation = other.m_reservation;
    m_accepted = other.m_accepted;
    return *this;
}

void GpuRetireRegistry::PreparedBatch::reset() noexcept {
    if (!m_registry) return;
    if (m_accepted)
        m_registry->quarantinePrepared(*this);
    else
        m_registry->releasePrepared(*this);
}

void GpuRetireRegistry::noteSignalFailure() const {
    QMutexLocker locker(&preparedMutex());
    ++preparedMetrics().signalFailureCount;
}

#ifdef OLR_UNIT_TEST
void GpuRetireRegistry::failNextPreparationAllocationForTest() noexcept {
    failNextPreparationAllocation().store(true, std::memory_order_relaxed);
}

uint64_t GpuRetireRegistry::preparationAllocationCountForTest() noexcept {
    return preparationAllocations().load(std::memory_order_relaxed);
}
#endif
