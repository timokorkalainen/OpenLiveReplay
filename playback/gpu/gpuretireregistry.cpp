#include "playback/gpu/gpuretireregistry.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <atomic>
#include <utility>

namespace {

constexpr qsizetype kInlineOwnerCount = 4;
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

std::atomic<uint64_t>& nextReservation() {
    static std::atomic<uint64_t> reservation{1};
    return reservation;
}

} // namespace

#ifdef OLR_UNIT_TEST
void GpuRetireRegistry::registerRetire(std::shared_ptr<GpuSurface> surface,
                                       GpuRetirementTicket ticket) const {
    const std::shared_ptr<GpuFence> fence = ticket.fence();
    if (!surface || !fence || ticket.value() == 0) return;
    try {
        if (!fence->validatesRetirement(ticket, surface->compatibility())) return;
    } catch (...) {
        return;
    }
    const uint64_t reservation = gpuSubmissionDetail::takeMonotonicInstanceId(nextReservation());
    if (reservation == 0) return;
    const std::shared_ptr<GpuSurface> owner = surface;
    const GpuRetirePreparedHandle prepared =
        GpuReadbackRetainer::prepare(&owner, 1, fence, reservation);
    if (!prepared) return;
    surface->retainUntilFenceRetired(ticket.value());
    if (!GpuReadbackRetainer::publish(prepared, ticket)) GpuReadbackRetainer::release(prepared);
}
#endif

void GpuRetireRegistry::drainCompleted() const {
    GpuReadbackRetainer::drainCompleted();
}

qsizetype GpuRetireRegistry::pendingRetainCount() const {
    GpuReadbackRetainer::drainCompleted();
    return GpuReadbackRetainer::pendingCount();
}

qsizetype GpuRetireRegistry::abandonAllNoWait(const DeadDeviceToken& deadDevice) const {
    return GpuReadbackRetainer::abandonAllNoWait(deadDevice);
}

qsizetype
GpuRetireRegistry::abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices) const {
    return GpuReadbackRetainer::abandonAllNoWait(deadDevices);
}

int GpuRetireRegistry::drainWithBoundedWait(int totalTimeoutMs) const {
    return GpuReadbackRetainer::drainWithBoundedWait(totalTimeoutMs);
}

GpuRetireDiagnostics GpuRetireRegistry::diagnostics() const {
    GpuReadbackRetainer::drainCompleted();
    return GpuRetireDiagnostics{
        GpuReadbackRetainer::pendingCount(), GpuReadbackRetainer::highWaterMark(),
        GpuReadbackRetainer::timeoutCount(), GpuReadbackRetainer::signalFailureCount(),
        GpuReadbackRetainer::quarantineCount()};
}

GpuRetireRegistry::PreparedBatch
GpuRetireRegistry::prepareRetirement(const std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                                     const std::shared_ptr<GpuFence>& fence) const noexcept {
    if (!surfaces || count <= 0 || !fence) return {};

#ifdef OLR_UNIT_TEST
    if (count > kInlineOwnerCount) {
        auto& metrics = allocationProbeMetrics();
        if (metrics.failNext.exchange(false, std::memory_order_relaxed)) return {};
        metrics.preparation.fetch_add(1, std::memory_order_relaxed);
    }
#endif

    const uint64_t reservation = gpuSubmissionDetail::takeMonotonicInstanceId(nextReservation());
    if (reservation == 0) return {};
    const GpuRetirePreparedHandle prepared =
        GpuReadbackRetainer::prepare(surfaces, count, fence, reservation);
    if (!prepared) return {};
    return PreparedBatch(this, prepared);
}

bool GpuRetireRegistry::publishPrepared(PreparedBatch& prepared,
                                        GpuRetirementTicket ticket) const noexcept {
    if (!prepared || prepared.m_registry != this) return false;
    if (!GpuReadbackRetainer::publish(prepared.m_handle, ticket)) return false;
    prepared.m_registry = nullptr;
    return true;
}

void GpuRetireRegistry::quarantinePrepared(PreparedBatch& prepared) const noexcept {
    if (!prepared || prepared.m_registry != this) return;
    GpuReadbackRetainer::quarantine(prepared.m_handle);
    prepared.m_registry = nullptr;
}

void GpuRetireRegistry::releasePrepared(PreparedBatch& prepared) const noexcept {
    if (!prepared || prepared.m_registry != this) return;
    GpuReadbackRetainer::release(prepared.m_handle);
    prepared.m_registry = nullptr;
}

GpuRetireRegistry::PreparedBatch::~PreparedBatch() {
    reset();
}

GpuRetireRegistry::PreparedBatch::PreparedBatch(PreparedBatch&& other) noexcept
    : m_registry(std::exchange(other.m_registry, nullptr)), m_handle(other.m_handle),
      m_accepted(other.m_accepted) {}

GpuRetireRegistry::PreparedBatch&
GpuRetireRegistry::PreparedBatch::operator=(PreparedBatch&& other) noexcept {
    if (this == &other) return *this;
    reset();
    m_registry = std::exchange(other.m_registry, nullptr);
    m_handle = other.m_handle;
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

void GpuRetireRegistry::noteSignalFailure() const noexcept {
    GpuReadbackRetainer::noteSignalFailure();
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
    auto& metrics = allocationProbeMetrics();
    metrics.preparation.store(0, std::memory_order_relaxed);
    metrics.callback.store(0, std::memory_order_relaxed);
    metrics.postAccept.store(0, std::memory_order_relaxed);
    metrics.failNext.store(false, std::memory_order_relaxed);
}

GpuRetireAllocationSnapshot GpuRetireRegistry::allocationSnapshotForTest() noexcept {
    const auto& metrics = allocationProbeMetrics();
    return GpuRetireAllocationSnapshot{metrics.preparation.load(std::memory_order_relaxed),
                                       metrics.callback.load(std::memory_order_relaxed),
                                       metrics.postAccept.load(std::memory_order_relaxed)};
}

void GpuRetireRegistry::resetStorageProbeForTest() noexcept {
    GpuReadbackRetainer::resetStorageProbeForTest();
}

GpuRetireStorageSnapshot GpuRetireRegistry::storageSnapshotForTest() noexcept {
    return GpuReadbackRetainer::storageSnapshotForTest();
}

size_t GpuRetireRegistry::poolCapacityPerShardForTest() noexcept {
    return GpuReadbackRetainer::poolCapacityPerShardForTest();
}
#endif
