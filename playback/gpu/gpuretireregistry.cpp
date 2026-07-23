#include "playback/gpu/gpuretireregistry.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpusurface.h"

#include <atomic>
#include <memory_resource>
#include <new>
#include <utility>

namespace {

thread_local uint8_t currentAllocationPhase = 0;
#ifdef OLR_UNIT_TEST
thread_local bool allocationProbeEnabledForThread = true;
#endif

#ifdef OLR_UNIT_TEST
struct AllocationProbeMetrics {
    std::atomic<uint64_t> preparation{0};
    std::atomic<uint64_t> callback{0};
    std::atomic<uint64_t> postAccept{0};
    std::atomic<bool> failNext{false};
    std::atomic<uint8_t> injectPhase{0};
};

AllocationProbeMetrics& allocationProbeMetrics() {
    static AllocationProbeMetrics metrics;
    return metrics;
}

class RetirementMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto& metrics = allocationProbeMetrics();
        if (metrics.failNext.exchange(false, std::memory_order_relaxed)) throw std::bad_alloc();
        void* allocation = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        if (currentAllocationPhase == uint8_t(GpuRetireAllocationPhase::Preparation))
            metrics.preparation.fetch_add(1, std::memory_order_relaxed);
        else if (currentAllocationPhase == uint8_t(GpuRetireAllocationPhase::Callback))
            metrics.callback.fetch_add(1, std::memory_order_relaxed);
        else if (currentAllocationPhase == uint8_t(GpuRetireAllocationPhase::PostAccept))
            metrics.postAccept.fetch_add(1, std::memory_order_relaxed);
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

bool exerciseInjectedHeapEvent(uint8_t phase) noexcept {
    if (!allocationProbeEnabledForThread) return true;
    auto& metrics = allocationProbeMetrics();
    uint8_t expectedPhase = phase;
    const bool injected = metrics.injectPhase.compare_exchange_strong(
        expectedPhase, 0, std::memory_order_relaxed, std::memory_order_relaxed);
    const bool failingPreparation = phase == uint8_t(GpuRetireAllocationPhase::Preparation) &&
                                    metrics.failNext.load(std::memory_order_relaxed);
    if (!injected && !failingPreparation) return true;
    try {
        void* allocation = retirementMemoryResource().allocate(64, alignof(std::max_align_t));
        retirementMemoryResource().deallocate(allocation, 64, alignof(std::max_align_t));
        return true;
    } catch (...) {
        return false;
    }
}
#endif

std::atomic<uint64_t>& nextReservation() {
    static std::atomic<uint64_t> reservation{1};
    return reservation;
}

} // namespace

#ifdef OLR_UNIT_TEST
void GpuRetireRegistry::registerRetire(
    // The by-value parameters pin both owners for the complete registration transaction.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    std::shared_ptr<GpuSurface> surface,
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    GpuRetirementTicket ticket) const {
    const std::shared_ptr<GpuFence>& fence = ticket.fence();
    if (!surface || !fence || ticket.value() == 0) return;
    try {
        if (!fence->validatesRetirement(ticket, surface->compatibility())) return;
    } catch (...) {
        return;
    }
    const uint64_t reservation = gpuSubmissionDetail::takeMonotonicInstanceId(nextReservation());
    if (reservation == 0) return;
    const GpuRetirePreparedHandle prepared =
        GpuReadbackRetainer::prepare(&surface, 1, fence, reservation);
    if (!prepared) return;
    if (GpuReadbackRetainer::publish(prepared, ticket)) {
        try {
            surface->retainUntilFenceRetired(ticket.value());
        } catch (...) {
            // This compatibility helper is test-only; publication already pins the owner.
            static_cast<void>(0);
        }
    } else {
        GpuReadbackRetainer::release(prepared);
    }
}
#endif

void GpuRetireRegistry::drainCompleted() const {
    GpuReadbackRetainer::drainCompleted();
}

qsizetype GpuRetireRegistry::pendingRetainCount() const {
    GpuReadbackRetainer::drainCompleted();
    return GpuReadbackRetainer::pendingCount();
}

qsizetype GpuRetireRegistry::abandonAllNoWait(const GpuValidatedDeadDomains& deadDomains) const {
    return GpuReadbackRetainer::abandonAllNoWait(deadDomains);
}

int GpuRetireRegistry::drainWithBoundedWait(int totalTimeoutMs) const {
    return GpuReadbackRetainer::drainWithBoundedWait(totalTimeoutMs);
}

GpuRetireDiagnostics GpuRetireRegistry::diagnostics() const {
    GpuReadbackRetainer::drainCompleted();
    const GpuRetireMetricsSnapshot snapshot = GpuReadbackRetainer::diagnosticsSnapshot();
    return GpuRetireDiagnostics{snapshot.pendingOwners, snapshot.highWaterMark,
                                snapshot.timeoutCount, snapshot.signalFailureCount,
                                snapshot.quarantineOwners};
}

GpuRetireRegistry::PreparedBatch
GpuRetireRegistry::prepareRetirement(const std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                                     const std::shared_ptr<GpuFence>& fence) const noexcept {
    if (!surfaces || count <= 0 || !fence) return {};

#ifdef OLR_UNIT_TEST
    if (!exerciseInjectedHeapEvent(uint8_t(GpuRetireAllocationPhase::Preparation))) return {};
#endif

    const uint64_t reservation = gpuSubmissionDetail::takeMonotonicInstanceId(nextReservation());
    if (reservation == 0) return {};
    const GpuRetirePreparedHandle prepared =
        GpuReadbackRetainer::prepare(surfaces, count, fence, reservation);
    if (!prepared) return {};
    return PreparedBatch(this, prepared);
}

bool GpuRetireRegistry::publishPrepared(
    PreparedBatch& prepared,
    // The by-value ticket makes this an explicit ownership/consumption boundary.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
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
#ifdef OLR_UNIT_TEST
    (void) exerciseInjectedHeapEvent(uint8_t(GpuRetireAllocationPhase::Callback));
#endif
}

void GpuRetireRegistry::AllocationPhaseScope::enterPostAccept() noexcept {
    (void) GpuRetireRegistry::exchangeAllocationPhase(3);
#ifdef OLR_UNIT_TEST
    (void) exerciseInjectedHeapEvent(uint8_t(GpuRetireAllocationPhase::PostAccept));
#endif
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

void GpuRetireRegistry::injectHeapAllocationForNextPhaseForTest(
    GpuRetireAllocationPhase phase) noexcept {
    allocationProbeMetrics().injectPhase.store(uint8_t(phase), std::memory_order_relaxed);
}

void GpuRetireRegistry::resetAllocationProbeForTest() noexcept {
    auto& metrics = allocationProbeMetrics();
    metrics.preparation.store(0, std::memory_order_relaxed);
    metrics.callback.store(0, std::memory_order_relaxed);
    metrics.postAccept.store(0, std::memory_order_relaxed);
    metrics.failNext.store(false, std::memory_order_relaxed);
    metrics.injectPhase.store(0, std::memory_order_relaxed);
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

void GpuRetireRegistry::setStorageProbeEnabledForTest(bool enabled) noexcept {
    allocationProbeEnabledForThread = enabled;
    GpuReadbackRetainer::setStorageProbeEnabledForTest(enabled);
}

void GpuRetireRegistry::setDiagnosticsHookForTest(GpuRetireDiagnosticsHook hook,
                                                  void* context) noexcept {
    GpuReadbackRetainer::setDiagnosticsHookForTest(hook, context);
}

#endif
