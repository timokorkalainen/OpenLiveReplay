#include "playback/gpu/gpuretireregistry.h"

#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#include <array>
#include <atomic>
#include <memory_resource>
#include <new>
#include <optional>
#include <utility>

namespace {

constexpr size_t kPreparedSlotCount = 256;
constexpr qsizetype kInlineOwnerCount = 4;

enum class PreparedState : uint8_t { Free, Prepared, Signaled, Quarantined };

thread_local uint8_t currentAllocationPhase = 0;

#ifdef OLR_UNIT_TEST
struct AllocationProbeMetrics {
    std::atomic<uint64_t> preparation{0};
    std::atomic<uint64_t> callback{0};
    std::atomic<uint64_t> postAccept{0};
    std::atomic<bool> failNext{false};
};

AllocationProbeMetrics& allocationProbeMetrics() {
    static AllocationProbeMetrics metrics;
    return metrics;
}
#endif

class RetirementMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(size_t bytes, size_t alignment) override {
#ifdef OLR_UNIT_TEST
        auto& metrics = allocationProbeMetrics();
        if (metrics.failNext.exchange(false, std::memory_order_relaxed)) throw std::bad_alloc();
#endif
        void* allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
#ifdef OLR_UNIT_TEST
        if (currentAllocationPhase == 1)
            metrics.preparation.fetch_add(1, std::memory_order_relaxed);
        else if (currentAllocationPhase == 2)
            metrics.callback.fetch_add(1, std::memory_order_relaxed);
        else if (currentAllocationPhase == 3)
            metrics.postAccept.fetch_add(1, std::memory_order_relaxed);
#endif
        return allocation;
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

int drainPreparedWithBoundedWait(int totalTimeoutMs) {
    struct DrainProbe {
        size_t slot = 0;
        uint64_t reservation = 0;
        PreparedState state = PreparedState::Free;
        std::shared_ptr<GpuFence> fence;
        uint64_t value = 0;
        uint64_t gpuGeneration = 0;
        GpuFenceIdentity identity;
    };
    std::array<DrainProbe, kPreparedSlotCount> probes;
    size_t probeCount = 0;
    {
        QMutexLocker locker(&preparedMutex());
        const auto& entries = preparedSlots();
        for (size_t i = 0; i < entries.size(); ++i) {
            const PreparedSlot& slot = entries[i];
            if (slot.state == PreparedState::Free || slot.state == PreparedState::Prepared)
                continue;
            DrainProbe& probe = probes[probeCount++];
            probe.slot = i;
            probe.reservation = slot.reservation;
            probe.state = slot.state;
            probe.fence = slot.fence;
            if (slot.ticket) {
                probe.value = slot.ticket->value();
                probe.gpuGeneration = slot.ticket->gpuGeneration();
                probe.identity = slot.ticket->identity();
            }
        }
    }

    std::array<bool, kPreparedSlotCount> release{};
    uint64_t timedOut = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    for (size_t i = 0; i < probeCount; ++i) {
        const DrainProbe& probe = probes[i];
        // Quarantine has no completion ticket. Only matching authoritative
        // dead-domain abandonment may release these accepted owners.
        if (probe.state == PreparedState::Quarantined) continue;
        const bool liveCompatible = probe.state == PreparedState::Signaled && probe.fence &&
                                    probe.value != 0 && probe.gpuGeneration != 0 &&
                                    probe.identity == probe.fence->identity();
        if (!liveCompatible) {
            release[i] = true;
            continue;
        }

        bool retired = probe.fence->completedValue() >= probe.value;
        const int remainingMs = qMax(0, totalTimeoutMs - int(elapsed.elapsed()));
        if (!retired && remainingMs > 0) retired = probe.fence->wait(probe.value, remainingMs);
        release[i] = retired;
        if (!retired) ++timedOut;
    }

    int released = 0;
    QMutexLocker locker(&preparedMutex());
    auto& entries = preparedSlots();
    auto& metrics = preparedMetrics();
    metrics.timeoutCount += timedOut;
    for (size_t i = 0; i < probeCount; ++i) {
        if (!release[i]) continue;
        PreparedSlot& slot = entries[probes[i].slot];
        if (!slotMatches(slot, probes[i].reservation) || slot.state != probes[i].state) continue;
        metrics.pendingOwners -= slot.ownerCount;
        if (slot.state == PreparedState::Quarantined) metrics.quarantineOwners -= slot.ownerCount;
        released += int(slot.ownerCount);
        clearPreparedSlot(slot);
    }
    return released;
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
                                       GpuRetirementTicket ticket) const {
    GpuReadbackRetainer::registerRetire(std::move(surface), std::move(ticket));
}

void GpuRetireRegistry::drainCompleted() const {
    drainPreparedCompleted();
    GpuReadbackRetainer::drainCompleted();
}

qsizetype GpuRetireRegistry::pendingRetainCount() const {
    drainPreparedCompleted();
    return preparedPendingCount() + GpuReadbackRetainer::pendingCount();
}

qsizetype GpuRetireRegistry::abandonAllNoWait(const DeadDeviceToken& deadDevice) const {
    return abandonPreparedDomains(std::vector<DeadDeviceToken>{deadDevice}) +
           GpuReadbackRetainer::abandonAllNoWait(deadDevice);
}

qsizetype
GpuRetireRegistry::abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) const {
    return abandonPreparedDomains(deadDevices) + GpuReadbackRetainer::abandonAllNoWait(deadDevices);
}

int GpuRetireRegistry::drainWithBoundedWait(int totalTimeoutMs) const {
    QElapsedTimer elapsed;
    elapsed.start();
    const int preparedReleased = drainPreparedWithBoundedWait(totalTimeoutMs);
    const int remainingMs = qMax(0, totalTimeoutMs - int(elapsed.elapsed()));
    return preparedReleased + GpuReadbackRetainer::drainWithBoundedWait(remainingMs);
}

GpuRetireDiagnostics GpuRetireRegistry::diagnostics() const {
    drainPreparedCompleted();
    const qsizetype legacyPending = GpuReadbackRetainer::pendingCount();
    const qsizetype legacyHighWater = GpuReadbackRetainer::highWaterMark();
    const uint64_t legacyTimeouts = GpuReadbackRetainer::timeoutCount();
    const uint64_t legacySignalFailures = GpuReadbackRetainer::signalFailureCount();
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

GpuRetireRegistry::AllocationPhaseScope::AllocationPhaseScope(uint8_t initialPhase) noexcept
    : m_previousPhase(GpuRetireRegistry::exchangeAllocationPhase(initialPhase)) {}

GpuRetireRegistry::AllocationPhaseScope::AllocationPhaseScope(AllocationPhaseScope&& other) noexcept
    : m_previousPhase(other.m_previousPhase), m_active(std::exchange(other.m_active, false)) {}

GpuRetireRegistry::AllocationPhaseScope::~AllocationPhaseScope() {
    if (m_active) (void) GpuRetireRegistry::exchangeAllocationPhase(m_previousPhase);
}

void GpuRetireRegistry::AllocationPhaseScope::enterCallback() noexcept {
    (void) GpuRetireRegistry::exchangeAllocationPhase(2);
}

void GpuRetireRegistry::AllocationPhaseScope::enterPostAccept() noexcept {
    (void) GpuRetireRegistry::exchangeAllocationPhase(3);
}

GpuRetireRegistry::AllocationPhaseScope GpuRetireRegistry::beginAllocationScope() const noexcept {
    return AllocationPhaseScope(1);
}

uint8_t GpuRetireRegistry::exchangeAllocationPhase(uint8_t phase) noexcept {
    return std::exchange(currentAllocationPhase, phase);
}

#ifdef OLR_UNIT_TEST
void GpuRetireRegistry::failNextStorageAllocationForTest() noexcept {
    allocationProbeMetrics().failNext.store(true, std::memory_order_relaxed);
}

void GpuRetireRegistry::resetAllocationProbeForTest() noexcept {
    QMutexLocker locker(&preparedMutex());
    for (PreparedSlot& slot : preparedSlots()) {
        if (slot.state == PreparedState::Free)
            slot.overflowOwners =
                std::pmr::vector<std::shared_ptr<GpuSurface>>(&retirementMemoryResource());
    }
    auto& metrics = allocationProbeMetrics();
    metrics.preparation.store(0, std::memory_order_relaxed);
    metrics.callback.store(0, std::memory_order_relaxed);
    metrics.postAccept.store(0, std::memory_order_relaxed);
    metrics.failNext.store(false, std::memory_order_relaxed);
}

GpuRetireAllocationSnapshot GpuRetireRegistry::allocationSnapshotForTest() noexcept {
    auto& metrics = allocationProbeMetrics();
    return GpuRetireAllocationSnapshot{metrics.preparation.load(std::memory_order_relaxed),
                                       metrics.callback.load(std::memory_order_relaxed),
                                       metrics.postAccept.load(std::memory_order_relaxed)};
}
#endif
