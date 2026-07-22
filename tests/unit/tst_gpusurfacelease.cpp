// Type-state gate for the GPU surface native handle (Challenge 3, part 1).
//
// The load-bearing guarantee is a NEGATIVE one, enforced at COMPILE time by the
// static_asserts below: GpuSurface::nativeHandle() is unreachable outside a
// GpuReadLease, and a DeadDeviceToken is unconstructible outside the two driver
// mints. If either gate regresses, this translation unit stops compiling — which is
// exactly the falsifiability the challenge asks for. The runtime slots then exercise
// the positive path (lease hands out the real handle; the bounded-wait drain releases
// only retired surfaces).
#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"
#include "playback/output/framepixelformat.h"

#include <memory>
#include <array>
#include <atomic>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch = capture(),
                            uintptr_t deviceDomainId = 0) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
            deviceDomainId);
    }
};

struct GpuRetirementTicketTestAuthority {
    static GpuRetirementTicket withFence(const GpuRetirementTicket& source,
                                         std::shared_ptr<GpuFence> fence) {
        GpuRetirementTicket clone(source);
        clone.m_fence = std::move(fence);
        return clone;
    }
    static GpuRetirementTicket withIdentity(const GpuRetirementTicket& source,
                                            GpuFenceIdentity identity) {
        GpuRetirementTicket clone(source);
        clone.m_identity = identity;
        return clone;
    }
    static GpuRetirementTicket withGeneration(const GpuRetirementTicket& source,
                                              uint64_t generation) {
        GpuRetirementTicket clone(source);
        clone.m_gpuGeneration = generation;
        return clone;
    }
    static GpuRetirementTicket withValue(const GpuRetirementTicket& source, uint64_t value) {
        GpuRetirementTicket clone(source);
        clone.m_value = value;
        return clone;
    }
};

struct GpuRetireRegistryTestAuthority {
    static qsizetype abandon(const GpuRetireRegistry& registry, const DeadDeviceToken& token) {
        const std::vector<DeadDeviceToken> tokens{token};
        const GpuValidatedDeadDomains domains(tokens);
        return registry.abandonAllNoWait(domains);
    }
    static qsizetype abandon(const GpuRetireRegistry& registry,
                             const std::vector<DeadDeviceToken>& tokens) {
        const GpuValidatedDeadDomains domains(tokens);
        return registry.abandonAllNoWait(domains);
    }
    static void registerRetire(const GpuRetireRegistry& registry,
                               std::shared_ptr<GpuSurface> surface, GpuRetirementTicket ticket) {
        registry.registerRetire(std::move(surface), std::move(ticket));
    }
    static auto prepare(const GpuRetireRegistry& registry,
                        const std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                        const std::shared_ptr<GpuFence>& fence) {
        return registry.prepareRetirement(surfaces, count, fence);
    }
    static GpuRetirePreparedHandle handle(const GpuRetireRegistry::PreparedBatch& prepared) {
        return prepared.m_handle;
    }
    static bool publish(const GpuRetireRegistry& registry,
                        GpuRetireRegistry::PreparedBatch& prepared, GpuRetirementTicket ticket) {
        return registry.publishPrepared(prepared, std::move(ticket));
    }
    static bool publishHandle(const GpuRetireRegistry& registry, GpuRetirePreparedHandle handle,
                              GpuRetirementTicket ticket) {
        GpuRetireRegistry::PreparedBatch prepared(&registry, handle);
        return registry.publishPrepared(prepared, std::move(ticket));
    }
};
#endif

namespace {

uint64_t currentDeviceAuthority() {
    return GpuDeviceLossMonitorTestAuthority::capture();
}

// SFINAE probe: is `s.nativeHandle()` well-formed from a non-friend context? Access
// checking is part of substitution (CWG1170), so a protected member yields a soft
// failure -> false_type. This is the compile-time negative-compile test: it lives in
// a normally-compiled TU, so a regression to a public nativeHandle() breaks the build.
template <typename T, typename = void>
struct CanCallNativeHandle : std::false_type {};
template <typename T>
struct CanCallNativeHandle<T, std::void_t<decltype(std::declval<const T&>().nativeHandle())>>
    : std::true_type {};

static_assert(!CanCallNativeHandle<GpuSurface>::value,
              "GpuSurface::nativeHandle() must be inaccessible without a GpuReadLease — "
              "the type-state gate is broken if this fires.");

// The device-loss no-wait free is gated on a DeadDeviceToken that only the driver
// mints can construct. Prove the injection path cannot fabricate one.
static_assert(!std::is_default_constructible<DeadDeviceToken>::value,
              "DeadDeviceToken must not be default-constructible (mint-only).");
static_assert(!std::is_constructible<DeadDeviceToken, DeadDeviceToken::Provenance, uint64_t>::value,
              "DeadDeviceToken's provenance constructor must be private (driver-mint-only).");
static_assert(!std::is_constructible<DeadDeviceToken, DeadDeviceToken::Provenance, uint64_t,
                                     uintptr_t>::value,
              "DeadDeviceToken's device-scoped constructor must be private (driver-mint-only).");
static_assert(!std::is_copy_constructible<GpuReadLease>::value,
              "GpuReadLease must not escape a synchronous callback by copy.");
static_assert(!std::is_move_constructible<GpuReadLease>::value,
              "GpuReadLease must not escape a synchronous callback by move.");
static_assert(!std::is_move_assignable<GpuReadLease>::value,
              "GpuReadLease registration must not be transferable by move assignment.");
static_assert(!std::is_move_constructible<GpuSyncReadScope>::value,
              "GpuSyncReadScope must not move while a lease is registered.");
static_assert(!std::is_move_assignable<GpuSyncReadScope>::value,
              "GpuSyncReadScope registration storage must have a stable address.");
static_assert(!std::is_default_constructible<GpuScopedNativeSurface>::value,
              "Only GpuOpScope may construct a scoped native surface.");
static_assert(!std::is_copy_constructible<GpuScopedNativeSurface>::value,
              "Scoped native surfaces must not escape by copy.");
static_assert(!std::is_move_constructible<GpuScopedNativeSurface>::value,
              "Scoped native surfaces must not escape by move.");
static_assert(!std::is_copy_assignable<GpuScopedNativeSurface>::value,
              "Scoped native surfaces must not escape by copy assignment.");
static_assert(!std::is_move_assignable<GpuScopedNativeSurface>::value,
              "Scoped native surfaces must not escape by move assignment.");
static_assert(!std::is_default_constructible<GpuScopedNativeView<1>>::value,
              "Only GpuOpScope may construct a scoped native view.");
static_assert(!std::is_copy_constructible<GpuScopedNativeView<1>>::value,
              "Scoped native views must not escape by copy.");
static_assert(!std::is_move_constructible<GpuScopedNativeView<1>>::value,
              "Scoped native views must not escape by move.");
static_assert(!std::is_copy_assignable<GpuScopedNativeView<1>>::value,
              "Scoped native views must not escape by copy assignment.");
static_assert(!std::is_move_assignable<GpuScopedNativeView<1>>::value,
              "Scoped native views must not escape by move assignment.");
static_assert(!std::is_copy_constructible<GpuOwnedNativeHandle>::value,
              "The raw-surface native owner must remain unique.");
static_assert(std::is_nothrow_move_constructible<GpuOwnedNativeHandle>::value,
              "The allocation-free native owner must transfer without throwing.");
static_assert(!std::is_default_constructible<GpuRetirementTicket>::value,
              "Retirement tickets must be fence-authority-minted.");
static_assert(!std::is_aggregate<GpuRetirementTicket>::value,
              "Retirement ticket fields must not be publicly aggregate-forgeable.");
static_assert(!std::is_constructible<GpuRetirementTicket, std::shared_ptr<GpuFence>,
                                     GpuFenceIdentity, uint64_t, uint64_t, uint64_t>::value,
              "Only fence authority may invoke the retirement-ticket constructor.");
static_assert(std::is_copy_constructible<GpuRetirementTicket>::value,
              "Authority tickets may be copied as immutable evidence.");
static_assert(!std::is_copy_assignable<GpuRetirementTicket>::value,
              "Copied retirement evidence must not be rewritable.");
static_assert(!std::is_move_assignable<GpuRetirementTicket>::value,
              "Moved retirement evidence must not be rewritable.");

// A surface whose native handle is a known sentinel. nativeHandle() is protected,
// mirroring the production surfaces, so the ONLY way the test reads it is via a lease.
class FakeLeaseSurface : public GpuSurface {
public:
    FakeLeaseSurface(void* handle, bool valid,
                     GpuSurfaceCompatibility compatibility =
                         GpuSurfaceCompatibility{0xF0, currentDeviceAuthority()},
                     GpuSurfaceDesc desc = {FramePixelFormat::Nv12, 16, 16, 0},
                     uint32_t subresource = 0)
        : m_handle(handle), m_valid(valid), m_compatibility(compatibility), m_desc(desc),
          m_subresource(subresource) {}
    GpuSurfaceDesc desc() const override { return m_desc; }
    bool isValid() const override { return m_valid; }
    GpuSurfaceCompatibility compatibility() const override { return m_compatibility; }

protected:
    void* nativeHandle() const override { return m_valid ? m_handle : nullptr; }
    uint32_t nativeSubresource() const override { return m_subresource; }

private:
    void* m_handle = nullptr;
    bool m_valid = false;
    GpuSurfaceCompatibility m_compatibility;
    GpuSurfaceDesc m_desc;
    uint32_t m_subresource = 0;
};

class ThrowingNativeHandleSurface final : public FakeLeaseSurface {
public:
    ThrowingNativeHandleSurface(GpuSurfaceCompatibility compatibility)
        : FakeLeaseSurface(reinterpret_cast<void*>(0xBAD), true, compatibility) {}

protected:
    void* nativeHandle() const override { throw std::runtime_error("native handle failure"); }
};

// Fence with a test-controllable completed watermark.
class FakeFence : public GpuFence {
public:
    explicit FakeFence(uintptr_t deviceDomainId = 0xF0)
        : GpuFence(deviceDomainId, currentDeviceAuthority()) {}
    FakeFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override {
        ++m_signalCalls;
        return ++m_signalled;
    }
    bool wait(uint64_t value, int /*timeoutMs*/) override {
        ++m_waitCalls;
        m_waitSawUnlockedRetainer = gpuRetireDetail::mutexAvailableForTest();
        if (m_retireOnWait) m_completed = value;
        return m_completed >= value;
    }
    uint64_t completedValue() const override {
        m_completedSawUnlockedRetainer = gpuRetireDetail::mutexAvailableForTest();
        ++m_completedCalls;
        return m_completed;
    }
    void setCompleted(uint64_t v) { m_completed = v; }
    void setRetireOnWait(bool retire) { m_retireOnWait = retire; }
    bool waitSawUnlockedRetainer() const { return m_waitSawUnlockedRetainer; }
    int signalCalls() const { return m_signalCalls; }
    int waitCalls() const { return m_waitCalls; }
    int completedCalls() const { return m_completedCalls; }
    bool completedSawUnlockedRetainer() const { return m_completedSawUnlockedRetainer; }

    template <typename SubmitFn>
    auto submitForTest(const GpuFenceIdentity& preparedFence,
                       const GpuSurfaceCompatibility& compatibility, uint64_t generation,
                       SubmitFn&& submitFn) {
        return submitExactForRetirement(preparedFence, compatibility, generation,
                                        std::forward<SubmitFn>(submitFn));
    }

private:
    uint64_t m_signalled = 0;
    uint64_t m_completed = 0;
    bool m_retireOnWait = false;
    bool m_waitSawUnlockedRetainer = false;
    int m_signalCalls = 0;
    int m_waitCalls = 0;
    mutable int m_completedCalls = 0;
    mutable bool m_completedSawUnlockedRetainer = false;
};

class ConcurrentFence final : public GpuFence {
public:
    ConcurrentFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}

    uint64_t signal() override { return m_signalled.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override {
        return m_completed.load(std::memory_order_acquire) >= value;
    }
    uint64_t completedValue() const override {
        m_completedCalls.fetch_add(1, std::memory_order_relaxed);
        return m_completed.load(std::memory_order_acquire);
    }

    void advanceToSignalled() noexcept {
        m_completed.store(m_signalled.load(std::memory_order_acquire), std::memory_order_release);
    }
    void resetCompletedCalls() noexcept { m_completedCalls.store(0, std::memory_order_relaxed); }
    int completedCalls() const noexcept { return m_completedCalls.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> m_signalled{0};
    std::atomic<uint64_t> m_completed{0};
    mutable std::atomic<int> m_completedCalls{0};
};

class CountingSurface final : public FakeLeaseSurface {
public:
    CountingSurface(void* handle, GpuSurfaceCompatibility compatibility,
                    std::atomic<int>* destructions)
        : FakeLeaseSurface(handle, true, compatibility), m_destructions(destructions) {}
    ~CountingSurface() override { m_destructions->fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<int>* m_destructions = nullptr;
};

struct ConcurrentBackendAdapter {
    std::atomic<int>* calls = nullptr;
    template <size_t N>
    GpuSubmitOutcome operator()(const GpuScopedNativeView<N>&) noexcept {
        calls->fetch_add(1, std::memory_order_relaxed);
        return GpuSubmitOutcome::Submitted;
    }
};

uint64_t registerLegacyRetire(const GpuRetireRegistry& registry,
                              const std::shared_ptr<FakeLeaseSurface>& surface,
                              const std::shared_ptr<FakeFence>& fence) {
    const auto ticket = fence->submitForTest(fence->identity(), surface->compatibility(),
                                             GpuGenerationCounter::instance().current(),
                                             []() noexcept { return true; });
    Q_ASSERT(ticket.has_value());
    const uint64_t value = ticket->value();
    GpuRetireRegistryTestAuthority::registerRetire(registry, surface, *ticket);
    return value;
}

class ZeroSignalFence final : public GpuFence {
public:
    explicit ZeroSignalFence(uintptr_t deviceDomainId = 0)
        : GpuFence(deviceDomainId, currentDeviceAuthority()) {}
    ZeroSignalFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override {
        ++m_signalCalls;
        return 0;
    }
    bool wait(uint64_t value, int) override { return m_completed >= value; }
    uint64_t completedValue() const override { return m_completed; }
    void retireQuarantine() { m_completed = std::numeric_limits<uint64_t>::max(); }
    int signalCalls() const { return m_signalCalls; }

private:
    uint64_t m_completed = 0;
    int m_signalCalls = 0;
};

class ThrowingSignalFence final : public GpuFence {
public:
    explicit ThrowingSignalFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override { throw std::runtime_error("injected post-accept signal failure"); }
    bool wait(uint64_t, int) override { return false; }
    uint64_t completedValue() const override { return 0; }
};

class DeadlineConsumingFence final : public GpuFence {
public:
    explicit DeadlineConsumingFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t, int timeoutMs) override {
        ++m_waitCalls;
        m_lastTimeoutMs = timeoutMs;
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() <= timeoutMs)
            QThread::yieldCurrentThread();
        return false;
    }
    uint64_t completedValue() const override { return m_completed; }
    void setCompleted(uint64_t value) { m_completed = value; }
    int waitCalls() const { return m_waitCalls; }
    int lastTimeoutMs() const { return m_lastTimeoutMs; }

private:
    uint64_t m_signalled = 0;
    uint64_t m_completed = 0;
    int m_waitCalls = 0;
    int m_lastTimeoutMs = 0;
};

class PartialProgressFence final : public GpuFence {
public:
    PartialProgressFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t value, int) override {
        ++m_waitCalls;
        m_completed = value > 0 ? value - 1 : 0;
        return false;
    }
    uint64_t completedValue() const override {
        ++m_completedCalls;
        return m_completed;
    }
    int waitCalls() const noexcept { return m_waitCalls; }
    int completedCalls() const noexcept { return m_completedCalls; }
    void completeTo(uint64_t value) noexcept { m_completed = value; }

private:
    uint64_t m_signalled = 0;
    mutable uint64_t m_completed = 0;
    mutable int m_completedCalls = 0;
    int m_waitCalls = 0;
};

class ThrowingEvidenceSurface final : public FakeLeaseSurface {
public:
    ThrowingEvidenceSurface() : FakeLeaseSurface(reinterpret_cast<void*>(0xBAD), true) {}
    GpuSurfaceCompatibility compatibility() const override {
        throw std::runtime_error("injected compatibility evidence failure");
    }
};

class SingleReadEvidenceSurface final : public FakeLeaseSurface {
public:
    SingleReadEvidenceSurface(uintptr_t deviceDomain, uint64_t authorityEpoch)
        : FakeLeaseSurface(reinterpret_cast<void*>(0xACCE), true,
                           GpuSurfaceCompatibility{deviceDomain, authorityEpoch}) {}
    GpuSurfaceCompatibility compatibility() const override {
        if (++m_compatibilityCalls > 1)
            throw std::runtime_error("post-accept compatibility re-read");
        return FakeLeaseSurface::compatibility();
    }
    int compatibilityCalls() const noexcept { return m_compatibilityCalls; }

private:
    mutable int m_compatibilityCalls = 0;
};

struct FakeBackendAdapter {
    GpuSubmitOutcome outcome = GpuSubmitOutcome::Submitted;
    int calls = 0;
    bool injectNextPreparationFailure = false;
    GpuRetireAllocationSnapshot allocationSnapshotAtCallback;

    template <size_t N>
    GpuSubmitOutcome operator()(const GpuScopedNativeView<N>&) noexcept {
        ++calls;
        allocationSnapshotAtCallback = GpuRetireRegistry::allocationSnapshotForTest();
        if (injectNextPreparationFailure) GpuRetireRegistry::failNextStorageAllocationForTest();
        return outcome;
    }
};

class ConstantValueFence final : public GpuFence {
public:
    ConstantValueFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
        : GpuFence(deviceDomainId, authorityEpoch) {}
    uint64_t signal() override { return 1; }
    bool wait(uint64_t value, int) override { return m_completed >= value; }
    uint64_t completedValue() const override {
        ++m_completedCalls;
        return m_completed;
    }
    void complete() noexcept { m_completed = 1; }
    void resetCompletedCalls() noexcept { m_completedCalls = 0; }
    int completedCalls() const noexcept { return m_completedCalls; }

private:
    uint64_t m_completed = 0;
    mutable int m_completedCalls = 0;
};

struct DiagnosticsShardHandoff {
    GpuRetireRegistry* registry = nullptr;
    const DeadDeviceToken* oldDomainToken = nullptr;
    std::shared_ptr<ZeroSignalFence> newFence;
    std::shared_ptr<GpuSurface> newSurface;
    qsizetype released = 0;
    GpuSubmissionResult submitted;
};

void handoffQuarantineBetweenShards(void* opaque) noexcept {
    auto& handoff = *static_cast<DiagnosticsShardHandoff*>(opaque);
    handoff.released =
        GpuRetireRegistryTestAuthority::abandon(*handoff.registry, *handoff.oldDomainToken);
    FakeBackendAdapter adapter;
    GpuOpScope operation(handoff.newFence, *handoff.registry);
    handoff.submitted = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{handoff.newSurface}));
}

static_assert(noexcept(std::declval<GpuOpScope&>().submit(std::declval<FakeBackendAdapter&>(),
                                                          std::declval<GpuSurfacePack<1>>())),
              "The fused submission boundary must not propagate post-accept exceptions.");

template <size_t N>
GpuSubmissionResult
submitSurfaceCount(GpuRetireRegistry& registry, const std::shared_ptr<FakeFence>& fence,
                   uintptr_t deviceDomain, uint64_t authorityEpoch, uintptr_t handleBase) {
    std::array<std::shared_ptr<GpuSurface>, N> surfaces;
    for (size_t i = 0; i < N; ++i)
        surfaces[i] = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(handleBase + i), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
    FakeBackendAdapter adapter;
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(adapter, GpuSurfacePack<N>(std::move(surfaces)));
    return result;
}

template <size_t OwnerCount>
void verifyMultiNodePoolExhaustionIsAtomic(uintptr_t deviceDomain, uint64_t authorityEpoch) {
    static_assert(OwnerCount > 5 && OwnerCount <= 17);
    constexpr size_t requiredNodes = (OwnerCount + 4) / 5;
    GpuRetireRegistry registry;
    auto fence = std::make_shared<ConcurrentFence>(deviceDomain, authorityEpoch);
    const size_t capacity = GpuRetireRegistry::poolCapacityPerShardForTest();
    const size_t fillCount = capacity - (requiredNodes - 1);
    std::atomic<int> callbacks{0};
    GpuRetireRegistry::resetStorageProbeForTest();

    for (size_t i = 0; i < fillCount; ++i) {
        auto surface = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(uintptr_t(0x80000 + deviceDomain * 0x1000 + i)), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
        ConcurrentBackendAdapter adapter{&callbacks};
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    }

    std::array<std::shared_ptr<GpuSurface>, OwnerCount> overflowOwners;
    for (size_t i = 0; i < overflowOwners.size(); ++i)
        overflowOwners[i] = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(uintptr_t(0x90000 + deviceDomain * 0x1000 + i)), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
    ConcurrentBackendAdapter overflowAdapter{&callbacks};
    GpuOpScope overflow(fence, registry);
    const auto overflowResult =
        overflow.submit(overflowAdapter, GpuSurfacePack<OwnerCount>(std::move(overflowOwners)));
    QCOMPARE(overflowResult.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(overflowResult.retirement, GpuRetirementDisposition::None);
    QCOMPARE(callbacks.load(std::memory_order_relaxed), int(fillCount));
    const auto storage = GpuRetireRegistry::storageSnapshotForTest();
    QCOMPARE(storage.poolNodeAcquisitions, uint64_t(fillCount));
    QCOMPARE(storage.poolExhaustions, uint64_t(1));

    fence->advanceToSignalled();
    registry.drainCompleted();
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
}

#ifndef QT_NO_DEBUG
bool isExpectedCheckedContractTermination(QProcess::ExitStatus status, int exitCode) {
#ifdef Q_OS_WIN
    constexpr quint32 windowsFailFastAssertionStatus = 0xC0000602u;
    Q_UNUSED(status);
    return static_cast<quint32>(exitCode) == windowsFailFastAssertionStatus;
#else
    Q_UNUSED(exitCode);
    return status == QProcess::CrashExit;
#endif
}

struct ChildProcessResult {
    bool started = false;
    bool finishedBeforeTimeout = false;
    bool timedOut = false;
    bool reaped = false;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    int exitCode = -1;
    QProcess::ProcessError processError = QProcess::UnknownError;
    QString errorString;
    QByteArray output;
};

ChildProcessResult runChildProcess(const QString& testFunction, const QByteArray& environmentName,
                                   int timeoutMs, const QByteArray& startupMarker = {}) {
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(environmentName), QStringLiteral("1"));
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {testFunction});

    ChildProcessResult result;
    result.started = child.waitForStarted(5000);
    if (result.started && !startupMarker.isEmpty()) {
        QElapsedTimer markerWait;
        markerWait.start();
        while (!result.output.contains(startupMarker) && child.state() != QProcess::NotRunning &&
               markerWait.elapsed() < 5000) {
            (void) child.waitForReadyRead(5000 - int(markerWait.elapsed()));
            result.output += child.readAll();
        }
    }
    if (result.started) {
        result.finishedBeforeTimeout = child.waitForFinished(timeoutMs);
        if (!result.finishedBeforeTimeout) {
            result.timedOut = true;
            child.terminate();
            if (!child.waitForFinished(250)) {
                child.kill();
                (void) child.waitForFinished(5000);
            }
        }
    }
    if (child.state() != QProcess::NotRunning) {
        child.kill();
        (void) child.waitForFinished(5000);
    }

    result.reaped = child.state() == QProcess::NotRunning;
    result.exitStatus = child.exitStatus();
    result.exitCode = child.exitCode();
    result.processError = child.error();
    result.errorString = child.errorString();
    result.output += child.readAll();
    return result;
}

bool isAcceptedCheckedContractDeath(const ChildProcessResult& result) {
    return result.started && result.finishedBeforeTimeout && !result.timedOut && result.reaped &&
           isExpectedCheckedContractTermination(result.exitStatus, result.exitCode);
}

QString childProcessDiagnostic(const ChildProcessResult& result) {
    return QStringLiteral("started=%1 finishedBeforeTimeout=%2 timedOut=%3 reaped=%4 "
                          "exitStatus=%5 exitCodeSigned=%6 exitCodeHex=0x%7 processError=%8 "
                          "errorString=%9 output=%10")
        .arg(result.started ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.finishedBeforeTimeout ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.timedOut ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(result.reaped ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(int(result.exitStatus))
        .arg(result.exitCode)
        .arg(qulonglong(static_cast<quint32>(result.exitCode)), 8, 16, QLatin1Char('0'))
        .arg(int(result.processError))
        .arg(result.errorString)
        .arg(QString::fromLocal8Bit(result.output));
}
#endif

} // namespace

class TestGpuSurfaceLease : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void callbackLeaseExposesMetadataOnly();
    void callbackLeaseReportsInvalidSurface();
    void withReadCompletesScope();
    void withReadCompletesDuringExceptionUnwinding();
    void handleQueryAfterCompletionFailsCheckedContract();
    void escapedLeaseFailsClosedAfterScopeDestruction();
    void repeatedAcquisitionFailsClosedInRelease();
    void leaseDestructionDeregistersBeforeScopeCompletion();
    void checkedContractDeathOracleRejectsUnrelatedExit();
    void deathControlTimeoutCapturesDiagnosticsAndReaps();
    void surfaceOwnerSurvivesUntilLeaseDestruction();
    void boundedWaitDrainReleasesOnlyRetired();
    void boundedWaitDoesNotHoldRetainerMutex();
    void registryDiagnosticsTrackHighWaterAndTimeouts();
    void registryRegistrationDoesNotPollDriver();
    void preparedFenceGroupSurvivesDrainCancelAndRejectsStaleToken();
    void multiNodePreparedBatchRejectsStaleAbaToken();
    void duplicateFenceValuesRemainOneExactFenceGroup();
    void preparedFenceGroupAndResultOwnExactFenceReferences();
    void shardedRegistryQueriesEachFenceOncePerDrain();
    void fullShardDistinctFencesDrainInLinearOperations();
    void shardedRegistryConcurrentPublishAndDrainIsExact();
    void boundedWaitRePollsPartialProgressAfterTimeout();
    void diagnosticsSnapshotIsExactAcrossDeterministicShardHandoff();
    void diagnosticsRemainConsistentDuringValidatedAbandon();
    void preparedPoolExhaustionRejectsBeforeDriverAcceptance();
    void multiNodePoolExhaustionIsAllOrNothing();
    void fusedSubmissionRejectsIncompatibleEvidenceBeforeCallback();
    void fusedSubmissionRejectsThrowingEvidenceBeforeCallback();
    void fusedSubmissionCachesEvidenceBeforeCallback();
    void fusedSubmissionRejectsNullAtEveryPackPosition();
    void fusedSubmissionCoalescesDuplicateOwners();
    void fusedSubmissionViewPreservesLogicalSlotsAndDuplicateIdentity();
    void fusedSubmissionScopedAccessFailureIsContained();
    void fusedSubmissionOutOfRangeSlotFailsClosed();
    void fusedSubmissionCancellationIsPreSubmitOnly();
    void fusedSubmissionOneToFourSurfacesDoNotAllocate();
    void fusedSubmissionFifthOwnerSpillsToFixedPoolWithoutHeap();
    void fusedSubmissionSupportsSixAndSeventeenOwnerBatches();
    void fusedSubmissionCoalescesDuplicatesAcrossNodeBoundaries();
    void allocationProbeCountsRealInjectedHeapEventsByPhase();
    void fusedSubmissionNotSubmittedReleasesPreparedOwners();
    void fusedSubmissionPublishesSubmittedOwners();
    void acceptedSubmissionPublishesWhenGenerationChangesInsideCallback();
    void fusedSubmissionPublishesSubmittedWithErrorOwners();
    void fusedSubmissionQuarantinesZeroSignal();
    void fusedSubmissionQuarantinesPostAcceptThrow();
    void fusedMultiNodeFailureStatesCoverEveryOwner();
    void fusedSubmissionAllPostAcceptOutcomesDoNotAllocate();
    void fusedSubmissionNeverConsumesPostAcceptAllocationFailure();
    void boundedWaitDrainsTask3CompletedAndThenPendingLiveDomains();
    void boundedWaitPollsEveryLegacyRecordAfterTask3ExhaustsBudget();
    void boundedWaitSkipsDeadTask3DomainAndWaitsLiveDomain();
    void boundedWaitStillWaitsLiveTask3DomainAfterGenerationChange();
    void boundedWaitRetainsTask3QuarantineUntilAuthoritativeAbandonment();
    void zeroSignalQuarantineReleasesAfterAuthoritativeUpgrade();
    void deadTokenAbandonsOnlyMatchingDeviceDomain();
    void multipleDeadTokensAbandonInOnePass();
    void copiedLossEvidenceCannotAbandonSameDomainReplacementAuthority();
    void authoritativeLossRejectsLateSubmissionAndAbandonsPreLossRecord();
    void staleCoherentAuthorityIsRejectedAfterRebuild();
    void submissionBoundaryRejectsEachMismatchBeforeCallback();
    void retirementTicketRejectsEachIsolatedMutation();
    void retirementTicketCopyIsImmutableAndKeepsAuthority();
    void forgedOldCompletedValueIsRejected();
};

void TestGpuSurfaceLease::cleanup() {
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::callbackLeaseExposesMetadataOnly() {
    auto sentinel = reinterpret_cast<void*>(0xBEEF);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, /*valid=*/true);
    {
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        const auto state = std::make_pair(lease.valid(), lease.desc().width);
        QVERIFY(state.first);
        QCOMPARE(state.second, 16);
        QCOMPARE(lease.nativeHandle(), sentinel);
        scope.complete();
    }
}

void TestGpuSurfaceLease::copiedLossEvidenceCannotAbandonSameDomainReplacementAuthority() {
    constexpr uintptr_t deviceDomain = 0x5A11;
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t oldAuthority = GpuDeviceLossMonitorTestAuthority::capture();

    auto oldFence = std::make_shared<ZeroSignalFence>(deviceDomain, oldAuthority);
    auto oldSurface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5A110), true,
                                           GpuSurfaceCompatibility{deviceDomain, oldAuthority});
    FakeBackendAdapter adapter;
    GpuOpScope oldOperation(oldFence, registry);
    QCOMPARE(oldOperation
                 .submit(adapter,
                         GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{oldSurface}))
                 .retirement,
             GpuRetirementDisposition::Quarantined);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(oldAuthority, deviceDomain) != 0);
    const std::vector<DeadDeviceToken> staleEvidence = monitor.realLossTokens();

    monitor.beginRebuild();
    const uint64_t replacementAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(replacementAuthority != oldAuthority);
    monitor.clearForRebuild();
    auto replacementFence = std::make_shared<ZeroSignalFence>(deviceDomain, replacementAuthority);
    auto replacementSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0x5A111), true,
        GpuSurfaceCompatibility{deviceDomain, replacementAuthority});
    GpuOpScope replacementOperation(replacementFence, registry);
    QCOMPARE(replacementOperation
                 .submit(adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{
                                      replacementSurface}))
                 .retirement,
             GpuRetirementDisposition::Quarantined);

    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, staleEvidence), qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), qsizetype(1));
    QVERIFY(replacementSurface.use_count() > 1);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(replacementAuthority, deviceDomain) != 0);
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, monitor.realLossTokens()),
             qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::authoritativeLossRejectsLateSubmissionAndAbandonsPreLossRecord() {
    constexpr uintptr_t deviceDomain = 0x5A12;
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    auto oldFence = std::make_shared<ZeroSignalFence>(deviceDomain, authority);
    auto oldSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0x5A120), true, GpuSurfaceCompatibility{deviceDomain, authority});
    FakeBackendAdapter oldAdapter;
    GpuOpScope oldOperation(oldFence, registry);
    QCOMPARE(oldOperation
                 .submit(oldAdapter,
                         GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{oldSurface}))
                 .retirement,
             GpuRetirementDisposition::Quarantined);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deviceDomain) != 0);

    auto lateFence = std::make_shared<FakeFence>(deviceDomain, authority);
    auto lateSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0x5A121), true, GpuSurfaceCompatibility{deviceDomain, authority});
    FakeBackendAdapter lateAdapter;
    GpuOpScope lateOperation(lateFence, registry);
    const GpuSubmissionResult late = lateOperation.submit(
        lateAdapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{lateSurface}));
    QCOMPARE(late.retirement, GpuRetirementDisposition::None);
    QCOMPARE(lateAdapter.calls, 0);
    QCOMPARE(registry.pendingRetainCount(), qsizetype(1));

    const GpuValidatedLossResult recovery = monitor.withValidatedDeadDomains(
        [&](const GpuValidatedDeadDomains& domains) { return registry.abandonAllNoWait(domains); });
    QCOMPARE(recovery.status, GpuValidatedLossStatus::Completed);
    QCOMPARE(recovery.abandoned, qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::staleCoherentAuthorityIsRejectedAfterRebuild() {
    constexpr uintptr_t deviceDomain = 0x5A13;
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t staleAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    monitor.recordLoss();
    monitor.beginRebuild();
    const uint64_t replacementAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    QVERIFY(replacementAuthority != staleAuthority);
    monitor.clearForRebuild();

    GpuRetireRegistry registry;
    auto staleFence = std::make_shared<FakeFence>(deviceDomain, staleAuthority);
    auto staleSurface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5A130), true,
                                           GpuSurfaceCompatibility{deviceDomain, staleAuthority});
    FakeBackendAdapter staleAdapter;
    GpuOpScope staleOperation(staleFence, registry);
    const GpuSubmissionResult stale = staleOperation.submit(
        staleAdapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{staleSurface}));
    QCOMPARE(stale.retirement, GpuRetirementDisposition::None);
    QCOMPARE(staleAdapter.calls, 0);
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));

    auto replacementFence = std::make_shared<FakeFence>(deviceDomain, replacementAuthority);
    auto replacementSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0x5A131), true,
        GpuSurfaceCompatibility{deviceDomain, replacementAuthority});
    FakeBackendAdapter replacementAdapter;
    GpuOpScope replacementOperation(replacementFence, registry);
    const GpuSubmissionResult replacement = replacementOperation.submit(
        replacementAdapter,
        GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{replacementSurface}));
    QCOMPARE(replacement.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(replacementAdapter.calls, 1);
    replacementFence->setCompleted(replacement.fenceValue);
    registry.drainCompleted();
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
}

void TestGpuSurfaceLease::callbackLeaseReportsInvalidSurface() {
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x1), /*valid=*/false);
    GpuSyncReadScope scope;
    const bool valid = scope.read(surface).valid();
    scope.complete();
    QVERIFY(!valid);
}

void TestGpuSurfaceLease::submissionBoundaryRejectsEachMismatchBeforeCallback() {
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t preparedDomain = 0xD011;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    const uint64_t generation = GpuGenerationCounter::instance().current();
    const GpuSurfaceCompatibility compatibility{preparedDomain, authorityEpoch};
    auto preparedFence = std::make_shared<FakeFence>(preparedDomain, authorityEpoch);
    auto sameDomainOtherFence = std::make_shared<FakeFence>(preparedDomain, authorityEpoch);
    int callbackCount = 0;
    auto callback = [&]() noexcept {
        ++callbackCount;
        return true;
    };

    GpuFenceIdentity wrongInstance = preparedFence->identity();
    ++wrongInstance.instanceId;
    QVERIFY(!preparedFence->submitForTest(wrongInstance, compatibility, generation, callback));

    GpuFenceIdentity wrongDomain = preparedFence->identity();
    ++wrongDomain.deviceDomainId;
    QVERIFY(!preparedFence->submitForTest(wrongDomain, compatibility, generation, callback));

    GpuFenceIdentity wrongAuthority = preparedFence->identity();
    ++wrongAuthority.authorityEpoch;
    QVERIFY(!preparedFence->submitForTest(wrongAuthority, compatibility, generation, callback));

    QVERIFY(!preparedFence->submitForTest(preparedFence->identity(), compatibility, generation + 1,
                                          callback));
    QVERIFY(!sameDomainOtherFence->submitForTest(preparedFence->identity(), compatibility,
                                                 generation, callback));
    QCOMPARE(callbackCount, 0);
    QCOMPARE(preparedFence->signalCalls(), 0);
    QCOMPARE(sameDomainOtherFence->signalCalls(), 0);

    const auto ticket = preparedFence->submitForTest(preparedFence->identity(), compatibility,
                                                     generation, callback);
    QVERIFY(ticket.has_value());
    QCOMPARE(callbackCount, 1);
    QCOMPARE(preparedFence->signalCalls(), 1);
    QCOMPARE(ticket->fence().get(), preparedFence.get());
    QVERIFY(ticket->identity() == preparedFence->identity());
    QCOMPARE(ticket->gpuGeneration(), generation);
    QCOMPARE(ticket->value(), uint64_t(1));
    QVERIFY(preparedFence->validatesRetirement(*ticket, compatibility));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::retirementTicketRejectsEachIsolatedMutation() {
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t domain = 0xD044;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    const uint64_t generation = GpuGenerationCounter::instance().current();
    const GpuSurfaceCompatibility compatibility{domain, authorityEpoch};
    auto fence = std::make_shared<FakeFence>(domain, authorityEpoch);
    auto otherFence = std::make_shared<FakeFence>(domain, authorityEpoch);
    auto accepted = []() noexcept { return true; };

    const auto oldTicket =
        fence->submitForTest(fence->identity(), compatibility, generation, accepted);
    const auto ticket =
        fence->submitForTest(fence->identity(), compatibility, generation, accepted);
    QVERIFY(oldTicket.has_value());
    QVERIFY(ticket.has_value());
    QVERIFY(fence->validatesRetirement(*ticket, compatibility));

    const auto wrongPointer = GpuRetirementTicketTestAuthority::withFence(*ticket, otherFence);
    QVERIFY(!fence->validatesRetirement(wrongPointer, compatibility));

    GpuFenceIdentity wrongInstanceIdentity = ticket->identity();
    ++wrongInstanceIdentity.instanceId;
    const auto wrongInstance =
        GpuRetirementTicketTestAuthority::withIdentity(*ticket, wrongInstanceIdentity);
    QVERIFY(!fence->validatesRetirement(wrongInstance, compatibility));

    GpuFenceIdentity wrongDomainIdentity = ticket->identity();
    ++wrongDomainIdentity.deviceDomainId;
    const auto wrongDomain =
        GpuRetirementTicketTestAuthority::withIdentity(*ticket, wrongDomainIdentity);
    QVERIFY(!fence->validatesRetirement(wrongDomain, compatibility));

    GpuFenceIdentity wrongAuthorityIdentity = ticket->identity();
    ++wrongAuthorityIdentity.authorityEpoch;
    const auto wrongAuthority =
        GpuRetirementTicketTestAuthority::withIdentity(*ticket, wrongAuthorityIdentity);
    QVERIFY(!fence->validatesRetirement(wrongAuthority, compatibility));

    const auto wrongGeneration =
        GpuRetirementTicketTestAuthority::withGeneration(*ticket, generation + 1);
    QVERIFY(!fence->validatesRetirement(wrongGeneration, compatibility));

    const auto oldCompletedValue =
        GpuRetirementTicketTestAuthority::withValue(*ticket, oldTicket->value());
    QVERIFY(!fence->validatesRetirement(oldCompletedValue, compatibility));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::retirementTicketCopyIsImmutableAndKeepsAuthority() {
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t domain = 0xD055;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    const uint64_t generation = GpuGenerationCounter::instance().current();
    const GpuSurfaceCompatibility compatibility{domain, authorityEpoch};
    auto fence = std::make_shared<FakeFence>(domain, authorityEpoch);
    const auto issued = fence->submitForTest(fence->identity(), compatibility, generation,
                                             []() noexcept { return true; });
    QVERIFY(issued.has_value());

    const GpuRetirementTicket copy(*issued);
    QCOMPARE(copy.fence().get(), issued->fence().get());
    QVERIFY(copy.identity() == issued->identity());
    QCOMPARE(copy.gpuGeneration(), issued->gpuGeneration());
    QCOMPARE(copy.value(), issued->value());
    QVERIFY(fence->validatesRetirement(copy, compatibility));
    GpuFenceIdentity detachedIdentity = copy.identity();
    ++detachedIdentity.instanceId;
    QVERIFY(copy.identity() == issued->identity());
    GpuGenerationCounter::instance().bump();
    QVERIFY(!fence->validatesRetirement(copy, compatibility));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::forgedOldCompletedValueIsRejected() {
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t domain = 0xF0123;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    const uint64_t generation = GpuGenerationCounter::instance().current();
    const GpuSurfaceCompatibility compatibility{domain, authorityEpoch};
    auto fence = std::make_shared<FakeFence>(domain, authorityEpoch);

    const auto oldTicket = fence->submitForTest(fence->identity(), compatibility, generation,
                                                []() noexcept { return true; });
    const auto currentTicket = fence->submitForTest(fence->identity(), compatibility, generation,
                                                    []() noexcept { return true; });
    QVERIFY(oldTicket.has_value());
    QVERIFY(currentTicket.has_value());
    const uint64_t oldCompletedValue = oldTicket->value();
    fence->setCompleted(oldCompletedValue);
    const GpuRetirementTicket forged =
        GpuRetirementTicketTestAuthority::withValue(*currentTicket, oldCompletedValue);

    QVERIFY(!fence->validatesRetirement(forged, compatibility));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::withReadCompletesScope() {
    auto sentinel = reinterpret_cast<void*>(0xCAFE);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, /*valid=*/true);

    bool observed = false;
    {
        GpuSyncReadScope scope;
        scope.withRead(surface, [&](const GpuReadLease& lease) {
            observed = lease.nativeHandle() == sentinel;
        });
    }

    QVERIFY(observed);
}

void TestGpuSurfaceLease::withReadCompletesDuringExceptionUnwinding() {
#ifndef QT_NO_DEBUG
    constexpr auto deathChildEnvironment = "OLR_GPU_SYNC_READ_THROW_DEATH_CHILD";
    constexpr auto callbackReachedThrowMarker = "withRead callback reached throw";
    if (qEnvironmentVariableIsSet(deathChildEnvironment)) {
        auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xFACE), true);
        GpuSyncReadScope scope;
        try {
            scope.withRead(surface, [](const GpuReadLease& lease) {
                (void) lease.nativeHandle();
                std::fputs("withRead callback reached throw\n", stderr);
                std::fflush(stderr);
                throw std::runtime_error("expected callback failure");
            });
        } catch (const std::runtime_error&) {
        }
        return;
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("withReadCompletesDuringExceptionUnwinding"),
                        QByteArray(deathChildEnvironment), 10000);
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY2(result.output.contains(callbackReachedThrowMarker), qPrintable(diagnostic));
    QVERIFY2(result.exitStatus == QProcess::NormalExit && result.exitCode == 0,
             qPrintable(diagnostic));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::handleQueryAfterCompletionFailsCheckedContract() {
#ifndef QT_NO_DEBUG
    constexpr auto deathChildEnvironment = "OLR_GPU_SYNC_READ_LEASE_DEATH_CHILD";
    if (qEnvironmentVariableIsSet(deathChildEnvironment)) {
        auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD00D), true);
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        scope.complete();
        (void) lease.nativeHandle();
        return;
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("handleQueryAfterCompletionFailsCheckedContract"),
                        QByteArray(deathChildEnvironment), 10000);
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY2(isAcceptedCheckedContractDeath(result), qPrintable(diagnostic));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::escapedLeaseFailsClosedAfterScopeDestruction() {
#ifdef QT_NO_DEBUG
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD1E), true);
    const GpuReadLease escaped = [&]() -> GpuReadLease {
        GpuSyncReadScope scope;
        return scope.read(surface);
    }();

    QVERIFY(escaped.valid());
    QCOMPARE(escaped.nativeHandle(), nullptr);
#else
    QSKIP("Release fail-closed behavior is compiled under QT_NO_DEBUG");
#endif
}

void TestGpuSurfaceLease::repeatedAcquisitionFailsClosedInRelease() {
#ifdef QT_NO_DEBUG
    auto firstSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xA11), true);
    auto secondSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xA12), true);
    GpuSyncReadScope scope;
    const GpuReadLease first = scope.read(firstSurface);
    const GpuReadLease second = scope.read(secondSurface);

    QVERIFY(!second.valid());
    QCOMPARE(first.nativeHandle(), nullptr);
    QCOMPARE(second.nativeHandle(), nullptr);
#else
    QSKIP("Release fail-closed behavior is compiled under QT_NO_DEBUG");
#endif
}

void TestGpuSurfaceLease::leaseDestructionDeregistersBeforeScopeCompletion() {
    auto sentinel = reinterpret_cast<void*>(0xD3E6);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, true);
    GpuSyncReadScope scope;
    {
        const GpuReadLease lease = scope.read(surface);
        QCOMPARE(lease.nativeHandle(), sentinel);
    }
    scope.complete();
}

void TestGpuSurfaceLease::checkedContractDeathOracleRejectsUnrelatedExit() {
#ifndef QT_NO_DEBUG
    constexpr auto unrelatedChildEnvironment = "OLR_GPU_SYNC_READ_UNRELATED_FAILURE_CHILD";
    if (qEnvironmentVariableIsSet(unrelatedChildEnvironment)) {
        std::fputs("unrelated child failure mutation\n", stderr);
        std::fflush(stderr);
        std::exit(7);
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("checkedContractDeathOracleRejectsUnrelatedExit"),
                        QByteArray(unrelatedChildEnvironment), 10000);
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY2(result.output.contains("unrelated child failure mutation"), qPrintable(diagnostic));
    QVERIFY2(result.started && result.finishedBeforeTimeout && !result.timedOut && result.reaped,
             qPrintable(diagnostic));
    QVERIFY2(result.exitStatus == QProcess::NormalExit, qPrintable(diagnostic));
    QVERIFY2(result.exitCode == 7, qPrintable(diagnostic));
    QVERIFY2(!isAcceptedCheckedContractDeath(result), qPrintable(diagnostic));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::deathControlTimeoutCapturesDiagnosticsAndReaps() {
#ifndef QT_NO_DEBUG
    constexpr auto timeoutChildEnvironment = "OLR_GPU_SYNC_READ_TIMEOUT_CHILD";
    constexpr auto timeoutOutputMarker = "timeout child output marker";
    if (qEnvironmentVariableIsSet(timeoutChildEnvironment)) {
        std::fputs("timeout child output marker\n", stderr);
        std::fflush(stderr);
        QThread::msleep(5000);
        std::exit(0);
    }

    const ChildProcessResult result =
        runChildProcess(QStringLiteral("deathControlTimeoutCapturesDiagnosticsAndReaps"),
                        QByteArray(timeoutChildEnvironment), 10, QByteArray(timeoutOutputMarker));
    QVERIFY(result.started);
    QVERIFY(result.timedOut);
    QVERIFY(result.reaped);
    QVERIFY(result.output.contains(timeoutOutputMarker));
    QVERIFY(!isAcceptedCheckedContractDeath(result));
    const QString diagnostic = childProcessDiagnostic(result);
    QVERIFY(diagnostic.contains(QStringLiteral("timedOut=true")));
    QVERIFY(diagnostic.contains(QStringLiteral("exitCodeSigned=")));
    QVERIFY(diagnostic.contains(QStringLiteral("exitCodeHex=0x")));
    QVERIFY(diagnostic.contains(QStringLiteral("processError=")));
    QVERIFY(diagnostic.contains(QStringLiteral("errorString=")));
    QVERIFY(diagnostic.contains(QString::fromLatin1(timeoutOutputMarker)));
#else
    QSKIP("Checked-contract assertions are disabled in this build");
#endif
}

void TestGpuSurfaceLease::surfaceOwnerSurvivesUntilLeaseDestruction() {
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xABCD), true);
    std::weak_ptr<FakeLeaseSurface> observer = surface;
    bool ownerSurvivedBeforeCompletion = false;
    bool ownerSurvivedAfterCompletion = false;

    GpuSyncReadScope scope;
    {
        const GpuReadLease lease = scope.read(surface);
        surface.reset();
        ownerSurvivedBeforeCompletion = !observer.expired();
        scope.complete();
        ownerSurvivedAfterCompletion = !observer.expired();
    }

    QVERIFY(ownerSurvivedBeforeCompletion);
    QVERIFY(ownerSurvivedAfterCompletion);
    QVERIFY(observer.expired());
}

void TestGpuSurfaceLease::boundedWaitDrainReleasesOnlyRetired() {
    // Track our own surface's shared ownership rather than the global count, so other
    // tests' retains cannot perturb the assertions.
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x2), /*valid=*/true,
                                           GpuSurfaceCompatibility{0xF0, currentDeviceAuthority()});
    auto fence = std::make_shared<FakeFence>();
    const long baseline = surface.use_count();

    GpuRetireRegistry registry;
    const uint64_t value = registerLegacyRetire(registry, surface, fence);
    QVERIFY(surface.use_count() > baseline); // the retainer holds a reference

    // Fence has not reached 5 -> bounded wait cannot retire it -> surface stays held.
    registry.drainWithBoundedWait(1);
    QVERIFY(surface.use_count() > baseline);

    // Fence retires -> the bounded-wait drain releases the surface.
    fence->setCompleted(value);
    registry.drainWithBoundedWait(1);
    QCOMPARE(surface.use_count(), baseline);
}

void TestGpuSurfaceLease::boundedWaitDoesNotHoldRetainerMutex() {
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x3), true,
                                           GpuSurfaceCompatibility{0xF0, currentDeviceAuthority()});
    auto fence = std::make_shared<FakeFence>();
    fence->setRetireOnWait(true);
    GpuRetireRegistry registry;
    (void) registerLegacyRetire(registry, surface, fence);

    QCOMPARE(registry.drainWithBoundedWait(1), 1);
    QVERIFY(fence->waitSawUnlockedRetainer());
}

void TestGpuSurfaceLease::registryDiagnosticsTrackHighWaterAndTimeouts() {
    GpuRetireRegistry registry;
    const GpuRetireDiagnostics before = registry.diagnostics();
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x4), true,
                                           GpuSurfaceCompatibility{0xF0, currentDeviceAuthority()});
    auto fence = std::make_shared<FakeFence>();
    const uint64_t value = registerLegacyRetire(registry, surface, fence);

    const GpuRetireDiagnostics registered = registry.diagnostics();
    QVERIFY(registered.pendingRetains >= 1);
    QVERIFY(registered.highWaterMark >= registered.pendingRetains);
    QCOMPARE(registry.drainWithBoundedWait(1), 0);
    QVERIFY(registry.diagnostics().timeoutCount >= before.timeoutCount + 1);

    fence->setCompleted(value);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::fusedSubmissionRejectsIncompatibleEvidenceBeforeCallback() {
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto fence = std::make_shared<FakeFence>(0x51, currentDeviceAuthority());
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5), true,
                                           GpuSurfaceCompatibility{0x52, currentDeviceAuthority()});
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const GpuSubmissionResult result = operation.submit(
        adapter, GpuSurfacePack<1>({std::array<std::shared_ptr<GpuSurface>, 1>{surface}}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::None);
    QCOMPARE(adapter.calls, 0);
    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuSurfaceLease::fusedSubmissionRejectsThrowingEvidenceBeforeCallback() {
    GpuRetireRegistry registry;
    auto fence = std::make_shared<FakeFence>(0x59, currentDeviceAuthority());
    auto surface = std::make_shared<ThrowingEvidenceSurface>();
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::None);
    QCOMPARE(adapter.calls, 0);
    QCOMPARE(fence->signalCalls(), 0);
}

void TestGpuSurfaceLease::fusedSubmissionCachesEvidenceBeforeCallback() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5A, authority);
    auto surface = std::make_shared<SingleReadEvidenceSurface>(0x5A, authority);
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(surface->compatibilityCalls(), 1);
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionRejectsNullAtEveryPackPosition() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5B, authority);
    for (size_t nullPosition = 0; nullPosition < 4; ++nullPosition) {
        std::array<std::shared_ptr<GpuSurface>, 4> surfaces;
        for (size_t i = 0; i < surfaces.size(); ++i) {
            if (i != nullPosition)
                surfaces[i] =
                    std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5B0 + i), true,
                                                       GpuSurfaceCompatibility{0x5B, authority});
        }
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(adapter, GpuSurfacePack<4>(std::move(surfaces)));
        QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
        QCOMPARE(result.retirement, GpuRetirementDisposition::None);
        QCOMPARE(adapter.calls, 0);
    }
    QCOMPARE(fence->signalCalls(), 0);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionCoalescesDuplicateOwners() {
    GpuOpScope::resetStampCountForTest();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5C, authority);
    auto first = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5C0), true,
                                                    GpuSurfaceCompatibility{0x5C, authority});
    auto second = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5C1), true,
                                                     GpuSurfaceCompatibility{0x5C, authority});
    FakeBackendAdapter adapter;
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter,
        GpuSurfacePack<4>(std::array<std::shared_ptr<GpuSurface>, 4>{first, first, second, first}));
    QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(adapter.calls, 1);
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);
    QCOMPARE(first->pendingFenceValue(), result.fenceValue);
    QCOMPARE(second->pendingFenceValue(), result.fenceValue);
    QCOMPARE(GpuOpScope::stampCountForTest(), size_t(2));
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionViewPreservesLogicalSlotsAndDuplicateIdentity() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5C2, authority);
    const GpuSurfaceDesc firstDesc{FramePixelFormat::Nv12, 32, 18, 864};
    const GpuSurfaceDesc secondDesc{FramePixelFormat::Rgba8, 24, 12, 1152};
    auto first =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5C20), true,
                                           GpuSurfaceCompatibility{0x5C2, authority}, firstDesc, 3);
    auto second = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5C21), true,
                                                     GpuSurfaceCompatibility{0x5C2, authority},
                                                     secondDesc, 7);

    int calls = 0;
    std::array<void*, 4> handles{};
    std::array<GpuSurfaceDesc, 4> descs{};
    std::array<uint32_t, 4> subresources{};
    std::array<bool, 4> valid{};
    auto adapter = [&](const GpuScopedNativeView<4>& view) noexcept {
        ++calls;
        for (size_t i = 0; i < view.size(); ++i) {
            handles[i] = view[i].nativeHandle();
            descs[i] = view[i].desc();
            subresources[i] = view[i].nativeSubresource();
            valid[i] = view[i].valid();
        }
        return GpuSubmitOutcome::Submitted;
    };
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter,
        GpuSurfacePack<4>(std::array<std::shared_ptr<GpuSurface>, 4>{first, first, second, first}));
    QCOMPARE(calls, 1);
    QCOMPARE(handles[0], reinterpret_cast<void*>(0x5C20));
    QCOMPARE(handles[1], reinterpret_cast<void*>(0x5C20));
    QCOMPARE(handles[2], reinterpret_cast<void*>(0x5C21));
    QCOMPARE(handles[3], reinterpret_cast<void*>(0x5C20));
    QCOMPARE(descs[0].width, firstDesc.width);
    QCOMPARE(descs[2].format, secondDesc.format);
    QCOMPARE(subresources[0], uint32_t(3));
    QCOMPARE(subresources[2], uint32_t(7));
    QVERIFY(valid[0]);
    QVERIFY(valid[2]);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), qsizetype(2));
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionScopedAccessFailureIsContained() {
    GpuOpScope::resetStampCountForTest();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5C3, authority);
    auto surface =
        std::make_shared<ThrowingNativeHandleSurface>(GpuSurfaceCompatibility{0x5C3, authority});
    int calls = 0;
    auto adapter = [&](const GpuScopedNativeView<1>& view) noexcept {
        ++calls;
        return view.get<0>().nativeHandle() ? GpuSubmitOutcome::Submitted
                                            : GpuSubmitOutcome::NotSubmitted;
    };
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QCOMPARE(calls, 1);
    QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(GpuOpScope::stampCountForTest(), size_t(0));
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionOutOfRangeSlotFailsClosed() {
    GpuOpScope::resetStampCountForTest();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5C4, authority);
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5C40), true,
                                                      GpuSurfaceCompatibility{0x5C4, authority});
    int calls = 0;
    auto adapter = [&](const GpuScopedNativeView<1>& view) noexcept {
        ++calls;
        return view[1].nativeHandle() ? GpuSubmitOutcome::Submitted
                                      : GpuSubmitOutcome::NotSubmitted;
    };
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QCOMPARE(calls, 1);
    QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(GpuOpScope::stampCountForTest(), size_t(0));
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionCancellationIsPreSubmitOnly() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x5D, authority);
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5D0), true,
                                                      GpuSurfaceCompatibility{0x5D, authority});
    FakeBackendAdapter cancelledAdapter;
    GpuOpScope cancelled(fence, registry);

    QVERIFY(!cancelled.submitted());
    QVERIFY(cancelled.cancel());
    QVERIFY(!cancelled.submitted());
    QVERIFY(!cancelled.cancel());
    const auto cancelledResult = cancelled.submit(
        cancelledAdapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QCOMPARE(cancelledResult.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(cancelledResult.retirement, GpuRetirementDisposition::None);
    QCOMPARE(cancelledAdapter.calls, 0);
    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);

    FakeBackendAdapter submittedAdapter;
    GpuOpScope submitted(fence, registry);
    const auto submittedResult = submitted.submit(
        submittedAdapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QVERIFY(submitted.submitted());
    QVERIFY(!submitted.cancel());
    QCOMPARE(submittedResult.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(fence->signalCalls(), 1);
    fence->setCompleted(submittedResult.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionOneToFourSurfacesDoNotAllocate() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    GpuRetireRegistry::resetAllocationProbeForTest();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x61, authority);
    const auto one = submitSurfaceCount<1>(registry, fence, 0x61, authority, 0x610);
    const auto two = submitSurfaceCount<2>(registry, fence, 0x61, authority, 0x620);
    const auto three = submitSurfaceCount<3>(registry, fence, 0x61, authority, 0x630);
    const auto four = submitSurfaceCount<4>(registry, fence, 0x61, authority, 0x640);

    for (const GpuSubmissionResult& result : {one, two, three, four}) {
        QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    }
    QCOMPARE(one.fenceValue, uint64_t(1));
    QCOMPARE(four.fenceValue, uint64_t(4));
    const GpuRetireAllocationSnapshot allocations = GpuRetireRegistry::allocationSnapshotForTest();
    QCOMPARE(allocations.preparation, uint64_t(0));
    QCOMPARE(allocations.callback, uint64_t(0));
    QCOMPARE(allocations.postAccept, uint64_t(0));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 10);
    fence->setCompleted(four.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionFifthOwnerSpillsToFixedPoolWithoutHeap() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    GpuRetireRegistry::resetAllocationProbeForTest();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x71, authority);
    std::array<std::shared_ptr<GpuSurface>, 5> surfaces;
    for (size_t i = 0; i < surfaces.size(); ++i)
        surfaces[i] = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x70 + i), true,
                                                         GpuSurfaceCompatibility{0x71, authority});
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(adapter, GpuSurfacePack<5>(surfaces));

    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(adapter.allocationSnapshotAtCallback.preparation, uint64_t(0));
    QCOMPARE(adapter.allocationSnapshotAtCallback.callback, uint64_t(0));
    QCOMPARE(adapter.allocationSnapshotAtCallback.postAccept, uint64_t(0));
    const GpuRetireAllocationSnapshot allocations = GpuRetireRegistry::allocationSnapshotForTest();
    QCOMPARE(allocations.preparation, uint64_t(0));
    QCOMPARE(allocations.callback, uint64_t(0));
    QCOMPARE(allocations.postAccept, uint64_t(0));
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionSupportsSixAndSeventeenOwnerBatches() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    GpuRetireRegistry::resetStorageProbeForTest();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x72, authority);

    const auto six = submitSurfaceCount<6>(registry, fence, 0x72, authority, 0x720);
    const auto seventeen = submitSurfaceCount<17>(registry, fence, 0x72, authority, 0x730);

    QCOMPARE(six.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(seventeen.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(fence->signalCalls(), 2);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 23);
    const auto storage = GpuRetireRegistry::storageSnapshotForTest();
    QCOMPARE(storage.poolNodeAcquisitions, uint64_t(6));
    fence->setCompleted(seventeen.fenceValue);
    const int completedBefore = fence->completedCalls();
    registry.drainCompleted();
    QCOMPARE(fence->completedCalls(), completedBefore + 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionCoalescesDuplicatesAcrossNodeBoundaries() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x73, authority);
    std::array<std::shared_ptr<GpuSurface>, 8> surfaces;
    for (size_t i = 0; i < 6; ++i)
        surfaces[i] =
            std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(uintptr_t(0x740 + i)), true,
                                               GpuSurfaceCompatibility{0x73, authority});
    surfaces[6] = surfaces[0];
    surfaces[7] = surfaces[5];
    FakeBackendAdapter adapter;
    GpuOpScope operation(fence, registry);

    const auto result = operation.submit(adapter, GpuSurfacePack<8>(std::move(surfaces)));

    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(adapter.calls, 1);
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 6);
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionNotSubmittedReleasesPreparedOwners() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x81, authority);
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x81), true,
                                                      GpuSurfaceCompatibility{0x81, authority});
    const long ownersBefore = surface.use_count();
    FakeBackendAdapter adapter;
    adapter.outcome = GpuSubmitOutcome::NotSubmitted;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::None);
    QVERIFY(!operation.submitted());
    QVERIFY(!operation.cancel());
    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(surface.use_count(), ownersBefore);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionPublishesSubmittedOwners() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0x91, authority);
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x91), true,
                                                      GpuSurfaceCompatibility{0x91, authority});
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::acceptedSubmissionPublishesWhenGenerationChangesInsideCallback() {
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
    constexpr uintptr_t domain = 0xD066;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    const uint64_t generation = GpuGenerationCounter::instance().current();
    auto fence = std::make_shared<FakeFence>(domain, authorityEpoch);
    auto surface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xD066), true, GpuSurfaceCompatibility{domain, authorityEpoch});
    std::weak_ptr<GpuSurface> weakSurface = surface;
    GpuRetireRegistry registry;
    auto adapter = [&](const auto&) noexcept {
        GpuGenerationCounter::instance().bump();
        return GpuSubmitOutcome::Submitted;
    };

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(GpuGenerationCounter::instance().current(), generation + 1);
    QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(result.fenceValue, uint64_t(1));
    QCOMPARE(surface->pendingFenceValue(), result.fenceValue);
    QVERIFY(!GpuDeviceLossMonitor::instance().isLost());

    surface.reset();
    QVERIFY(!weakSurface.expired());
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    QVERIFY(weakSurface.expired());
}

void TestGpuSurfaceLease::fusedSubmissionPublishesSubmittedWithErrorOwners() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0xA1, authority);
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xA1), true,
                                                      GpuSurfaceCompatibility{0xA1, authority});
    FakeBackendAdapter adapter;
    adapter.outcome = GpuSubmitOutcome::SubmittedWithError;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::SubmittedWithError);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QVERIFY(operation.submitted());
    QVERIFY(!operation.cancel());
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionQuarantinesZeroSignal() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t failuresBefore = registry.diagnostics().signalFailureCount;
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xB1), true,
                                                      GpuSurfaceCompatibility{0xB1, authority});
    auto fence = std::make_shared<ZeroSignalFence>(0xB1, authority);
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Quarantined);
    QVERIFY(operation.submitted());
    QVERIFY(!operation.cancel());
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QCOMPARE(registry.diagnostics().signalFailureCount, failuresBefore + 1);
    QVERIFY(registry.diagnostics().quarantineCount >= 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(!GpuDeviceLossMonitor::instance().realLossToken().has_value());

    GpuDeviceLossMonitorTestAuthority::publish(authority, 0xB1);
    const auto token = GpuDeviceLossMonitor::instance().realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(1));
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionQuarantinesPostAcceptThrow() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xC1), true,
                                                      GpuSurfaceCompatibility{0xC1, authority});
    const long ownersBefore = surface.use_count();
    auto fence = std::make_shared<ThrowingSignalFence>(0xC1, authority);
    FakeBackendAdapter adapter;

    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));

    QCOMPARE(result.outcome, GpuSubmitOutcome::Submitted);
    QCOMPARE(result.retirement, GpuRetirementDisposition::Quarantined);
    QVERIFY(operation.submitted());
    QVERIFY(!operation.cancel());
    QVERIFY(surface.use_count() > ownersBefore);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    GpuDeviceLossMonitorTestAuthority::publish(authority, 0xC1);
    const auto token = GpuDeviceLossMonitor::instance().realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(1));
    QCOMPARE(surface.use_count(), ownersBefore);
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedMultiNodeFailureStatesCoverEveryOwner() {
    GpuGenerationCounter::instance().resetForTest();
    auto& monitor = GpuDeviceLossMonitor::instance();
    GpuRetireRegistry registry;
    auto owners = [](uintptr_t domain, uint64_t epoch, uintptr_t base) {
        std::array<std::shared_ptr<GpuSurface>, 6> surfaces;
        for (size_t i = 0; i < surfaces.size(); ++i)
            surfaces[i] = std::make_shared<FakeLeaseSurface>(
                reinterpret_cast<void*>(base + i), true, GpuSurfaceCompatibility{domain, epoch});
        return surfaces;
    };

    {
        const uint64_t authority = currentDeviceAuthority();
        auto fence = std::make_shared<FakeFence>(0xC2, authority);
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        QVERIFY(operation.cancel());
        const auto result =
            operation.submit(adapter, GpuSurfacePack<6>(owners(0xC2, authority, 0xC200)));
        QCOMPARE(result.outcome, GpuSubmitOutcome::NotSubmitted);
        QCOMPARE(adapter.calls, 0);
        QCOMPARE(fence->signalCalls(), 0);
    }

    monitor.reset();
    {
        const uint64_t authority = currentDeviceAuthority();
        const qsizetype pendingBefore = registry.pendingRetainCount();
        auto fence = std::make_shared<FakeFence>(0xC3, authority);
        FakeBackendAdapter adapter;
        adapter.outcome = GpuSubmitOutcome::SubmittedWithError;
        GpuOpScope operation(fence, registry);
        const auto result =
            operation.submit(adapter, GpuSurfacePack<6>(owners(0xC3, authority, 0xC300)));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
        QCOMPARE(registry.pendingRetainCount(), pendingBefore + 6);
        QVERIFY(monitor.isLost());
        fence->setCompleted(result.fenceValue);
        registry.drainCompleted();
    }

    monitor.reset();
    {
        const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
        const qsizetype pendingBefore = registry.pendingRetainCount();
        auto fence = std::make_shared<ZeroSignalFence>(0xC4, authority);
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        const auto result =
            operation.submit(adapter, GpuSurfacePack<6>(owners(0xC4, authority, 0xC400)));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Quarantined);
        QCOMPARE(registry.pendingRetainCount(), pendingBefore + 6);
        GpuDeviceLossMonitorTestAuthority::publish(authority, 0xC4);
        const auto token = monitor.realLossToken();
        QVERIFY(token.has_value());
        QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(6));
    }

    monitor.reset();
    {
        const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
        const qsizetype pendingBefore = registry.pendingRetainCount();
        auto fence = std::make_shared<ThrowingSignalFence>(0xC5, authority);
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        const auto result =
            operation.submit(adapter, GpuSurfacePack<6>(owners(0xC5, authority, 0xC500)));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Quarantined);
        QCOMPARE(registry.pendingRetainCount(), pendingBefore + 6);
        GpuDeviceLossMonitorTestAuthority::publish(authority, 0xC5);
        const auto token = monitor.realLossToken();
        QVERIFY(token.has_value());
        QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(6));
    }

    monitor.reset();
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionNeverConsumesPostAcceptAllocationFailure() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0xD1, authority);
    auto first = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD1), true,
                                                    GpuSurfaceCompatibility{0xD1, authority});
    FakeBackendAdapter accepted;
    accepted.injectNextPreparationFailure = true;

    GpuOpScope acceptedOperation(fence, registry);
    const auto acceptedResult = acceptedOperation.submit(
        accepted, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{first}));
    QCOMPARE(acceptedResult.retirement, GpuRetirementDisposition::Published);

    std::array<std::shared_ptr<GpuSurface>, 5> overflow;
    for (size_t i = 0; i < overflow.size(); ++i)
        overflow[i] = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD2 + i), true,
                                                         GpuSurfaceCompatibility{0xD1, authority});
    FakeBackendAdapter rejected;
    GpuOpScope rejectedOperation(fence, registry);
    const auto rejectedResult = rejectedOperation.submit(rejected, GpuSurfacePack<5>(overflow));

    QCOMPARE(rejectedResult.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(rejectedResult.retirement, GpuRetirementDisposition::None);
    QCOMPARE(rejected.calls, 0);
    fence->setCompleted(acceptedResult.fenceValue);
    registry.drainCompleted();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fusedSubmissionAllPostAcceptOutcomesDoNotAllocate() {
    GpuGenerationCounter::instance().resetForTest();
    GpuDeviceLossMonitor::instance().reset();
    GpuRetireRegistry registry;

    auto verify = [&](const std::shared_ptr<GpuFence>& fence, uintptr_t domain,
                      GpuSubmitOutcome outcome, GpuRetirementDisposition disposition,
                      uint64_t* fenceValue = nullptr) {
        GpuRetireRegistry::resetAllocationProbeForTest();
        auto surface = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(domain), true,
            GpuSurfaceCompatibility{domain, fence->identity().authorityEpoch});
        FakeBackendAdapter adapter;
        adapter.outcome = outcome;
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.outcome, outcome);
        QCOMPARE(result.retirement, disposition);
        const GpuRetireAllocationSnapshot allocations =
            GpuRetireRegistry::allocationSnapshotForTest();
        QCOMPARE(allocations.preparation, uint64_t(0));
        QCOMPARE(allocations.callback, uint64_t(0));
        QCOMPARE(allocations.postAccept, uint64_t(0));
        if (fenceValue) *fenceValue = result.fenceValue;
    };

    auto submittedFence = std::make_shared<FakeFence>(0xC2, currentDeviceAuthority());
    uint64_t submittedValue = 0;
    verify(submittedFence, 0xC2, GpuSubmitOutcome::Submitted, GpuRetirementDisposition::Published,
           &submittedValue);
    submittedFence->setCompleted(submittedValue);
    registry.drainCompleted();

    GpuDeviceLossMonitor::instance().reset();
    auto submittedErrorFence = std::make_shared<FakeFence>(0xC3, currentDeviceAuthority());
    uint64_t submittedErrorValue = 0;
    verify(submittedErrorFence, 0xC3, GpuSubmitOutcome::SubmittedWithError,
           GpuRetirementDisposition::Published, &submittedErrorValue);
    submittedErrorFence->setCompleted(submittedErrorValue);
    registry.drainCompleted();

    GpuDeviceLossMonitor::instance().reset();
    const uint64_t zeroAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    verify(std::make_shared<ZeroSignalFence>(0xC4, zeroAuthority), 0xC4,
           GpuSubmitOutcome::Submitted, GpuRetirementDisposition::Quarantined);
    QCOMPARE(registry.drainWithBoundedWait(0), 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(zeroAuthority, 0xC4) != 0);
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(
                 registry, *GpuDeviceLossMonitor::instance().realLossToken()),
             qsizetype(1));

    GpuDeviceLossMonitor::instance().reset();
    const uint64_t throwingAuthority = GpuDeviceLossMonitorTestAuthority::capture();
    verify(std::make_shared<ThrowingSignalFence>(0xC5, throwingAuthority), 0xC5,
           GpuSubmitOutcome::Submitted, GpuRetirementDisposition::Quarantined);
    QCOMPARE(registry.drainWithBoundedWait(0), 0);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(throwingAuthority, 0xC5) != 0);
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(
                 registry, *GpuDeviceLossMonitor::instance().realLossToken()),
             qsizetype(1));
    GpuDeviceLossMonitor::instance().reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::boundedWaitDrainsTask3CompletedAndThenPendingLiveDomains() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto completedFence = std::make_shared<FakeFence>(0xD2, authority);
    auto pendingFence = std::make_shared<FakeFence>(0xD3, authority);
    auto completedSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xD20), true, GpuSurfaceCompatibility{0xD2, authority});
    auto pendingSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xD30), true, GpuSurfaceCompatibility{0xD3, authority});
    const long completedOwners = completedSurface.use_count();
    const long pendingOwners = pendingSurface.use_count();
    FakeBackendAdapter completedAdapter;
    FakeBackendAdapter pendingAdapter;
    GpuOpScope completedOperation(completedFence, registry);
    GpuOpScope pendingOperation(pendingFence, registry);
    const auto completedResult = completedOperation.submit(
        completedAdapter,
        GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{completedSurface}));
    const auto pendingResult = pendingOperation.submit(
        pendingAdapter,
        GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{pendingSurface}));
    completedFence->setCompleted(completedResult.fenceValue);

    QCOMPARE(registry.drainWithBoundedWait(1), 1);
    QCOMPARE(completedSurface.use_count(), completedOwners);
    QVERIFY(pendingSurface.use_count() > pendingOwners);
    QVERIFY(pendingFence->waitCalls() >= 1);

    pendingFence->setCompleted(pendingResult.fenceValue);
    QCOMPARE(registry.drainWithBoundedWait(1), 1);
    QCOMPARE(pendingSurface.use_count(), pendingOwners);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::boundedWaitPollsEveryLegacyRecordAfterTask3ExhaustsBudget() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    constexpr int timeoutMs = 2;
    const uint64_t authority = currentDeviceAuthority();

    auto task3Fence = std::make_shared<DeadlineConsumingFence>(0xD8, authority);
    auto task3Surface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xD80), true, GpuSurfaceCompatibility{0xD8, authority});
    const long task3Owners = task3Surface.use_count();
    FakeBackendAdapter adapter;
    GpuOpScope operation(task3Fence, registry);
    const auto task3Result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{task3Surface}));
    QCOMPARE(task3Result.retirement, GpuRetirementDisposition::Published);

    auto firstCompletedFence = std::make_shared<FakeFence>(0xD9, authority);
    auto pendingFence = std::make_shared<FakeFence>(0xDA, authority);
    auto lastCompletedFence = std::make_shared<FakeFence>(0xDB, authority);
    auto firstCompletedSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xD90), true, GpuSurfaceCompatibility{0xD9, authority});
    auto pendingSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xDA0), true, GpuSurfaceCompatibility{0xDA, authority});
    auto lastCompletedSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xDB0), true, GpuSurfaceCompatibility{0xDB, authority});
    const long firstCompletedOwners = firstCompletedSurface.use_count();
    const long pendingOwners = pendingSurface.use_count();
    const long lastCompletedOwners = lastCompletedSurface.use_count();
    const uint64_t firstCompletedValue =
        registerLegacyRetire(registry, firstCompletedSurface, firstCompletedFence);
    const uint64_t pendingValue = registerLegacyRetire(registry, pendingSurface, pendingFence);
    const uint64_t lastCompletedValue =
        registerLegacyRetire(registry, lastCompletedSurface, lastCompletedFence);
    const int firstCompletedPolls = firstCompletedFence->completedCalls();
    const int pendingPolls = pendingFence->completedCalls();
    const int lastCompletedPolls = lastCompletedFence->completedCalls();
    firstCompletedFence->setCompleted(firstCompletedValue);
    lastCompletedFence->setCompleted(lastCompletedValue);

    QCOMPARE(registry.drainWithBoundedWait(timeoutMs), 2);
    QCOMPARE(task3Fence->waitCalls(), 1);
    QCOMPARE(task3Fence->lastTimeoutMs(), timeoutMs);
    QCOMPARE(firstCompletedFence->completedCalls(), firstCompletedPolls + 1);
    QCOMPARE(pendingFence->completedCalls(), pendingPolls + 1);
    QCOMPARE(lastCompletedFence->completedCalls(), lastCompletedPolls + 1);
    QCOMPARE(firstCompletedFence->waitCalls(), 0);
    QCOMPARE(pendingFence->waitCalls(), 0);
    QCOMPARE(lastCompletedFence->waitCalls(), 0);
    QVERIFY(task3Surface.use_count() > task3Owners);
    QCOMPARE(firstCompletedSurface.use_count(), firstCompletedOwners);
    QVERIFY(pendingSurface.use_count() > pendingOwners);
    QCOMPARE(lastCompletedSurface.use_count(), lastCompletedOwners);

    task3Fence->setCompleted(task3Result.fenceValue);
    pendingFence->setCompleted(pendingValue);
    registry.drainCompleted();
    QCOMPARE(task3Surface.use_count(), task3Owners);
    QCOMPARE(pendingSurface.use_count(), pendingOwners);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::boundedWaitSkipsDeadTask3DomainAndWaitsLiveDomain() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    auto deadFence = std::make_shared<FakeFence>(0xD4, authority);
    auto liveFence = std::make_shared<FakeFence>(0xD5, authority);
    liveFence->setRetireOnWait(true);
    auto deadSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD40), true,
                                                          GpuSurfaceCompatibility{0xD4, authority});
    auto liveSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD50), true,
                                                          GpuSurfaceCompatibility{0xD5, authority});
    FakeBackendAdapter adapter;
    GpuOpScope deadOperation(deadFence, registry);
    GpuOpScope liveOperation(liveFence, registry);
    (void) deadOperation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{deadSurface}));
    (void) liveOperation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{liveSurface}));

    GpuDeviceLossMonitorTestAuthority::publish(authority, 0xD4);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(1));
    QCOMPARE(deadFence->waitCalls(), 0);
    QCOMPARE(registry.drainWithBoundedWait(5), 1);
    QCOMPARE(deadFence->waitCalls(), 0);
    QCOMPARE(liveFence->waitCalls(), 1);
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::boundedWaitStillWaitsLiveTask3DomainAfterGenerationChange() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0xD6, authority);
    fence->setRetireOnWait(true);
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD60), true,
                                                      GpuSurfaceCompatibility{0xD6, authority});
    const long ownersBefore = surface.use_count();
    FakeBackendAdapter adapter;
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    monitor.recordLoss();

    QCOMPARE(registry.drainWithBoundedWait(1), 1);
    QCOMPARE(fence->waitCalls(), 1);
    QCOMPARE(surface.use_count(), ownersBefore);
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::boundedWaitRetainsTask3QuarantineUntilAuthoritativeAbandonment() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    constexpr uintptr_t deviceDomain = 0xD7;
    auto zeroSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xD70), true, GpuSurfaceCompatibility{deviceDomain, authority});
    const long zeroOwners = zeroSurface.use_count();
    auto zeroFence = std::make_shared<ZeroSignalFence>(deviceDomain, authority);
    FakeBackendAdapter zeroAdapter;
    GpuOpScope zeroOperation(zeroFence, registry);
    const auto zeroResult = zeroOperation.submit(
        zeroAdapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{zeroSurface}));
    QCOMPARE(zeroResult.retirement, GpuRetirementDisposition::Quarantined);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);

    QCOMPARE(registry.drainWithBoundedWait(2), 0);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(zeroSurface.use_count() > zeroOwners);

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authority, deviceDomain) != 0);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QCOMPARE(zeroSurface.use_count(), zeroOwners);
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::zeroSignalQuarantineReleasesAfterAuthoritativeUpgrade() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t deviceAuthorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    constexpr uintptr_t deviceDomain = 0xA0;
    auto surface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0xA), true,
        GpuSurfaceCompatibility{deviceDomain, deviceAuthorityEpoch});
    const long ownersBefore = surface.use_count();
    auto fence = std::make_shared<ZeroSignalFence>(deviceDomain, deviceAuthorityEpoch);

    FakeBackendAdapter adapter;
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
    QCOMPARE(result.retirement, GpuRetirementDisposition::Quarantined);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(surface.use_count() > ownersBefore);

    const uint64_t lossGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(deviceAuthorityEpoch, deviceDomain);
    QVERIFY(lossGeneration != 0);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QCOMPARE(surface.use_count(), ownersBefore);
    monitor.reset();
}

void TestGpuSurfaceLease::deadTokenAbandonsOnlyMatchingDeviceDomain() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    auto deadSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xB), true,
                                                          GpuSurfaceCompatibility{11, authority});
    auto liveSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xC), true,
                                                          GpuSurfaceCompatibility{22, authority});
    const long deadOwners = deadSurface.use_count();
    const long liveOwners = liveSurface.use_count();
    auto deadFence = std::make_shared<FakeFence>(11, authority);
    auto liveFence = std::make_shared<FakeFence>(22, authority);
    GpuRetireRegistry registry;
    (void) registerLegacyRetire(registry, deadSurface, deadFence);
    const uint64_t liveValue = registerLegacyRetire(registry, liveSurface, liveFence);

    GpuDeviceLossMonitorTestAuthority::publish(authority, 11);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(token->deviceDomainId(), uintptr_t(11));
    GpuRetireRegistry::resetStorageProbeForTest();
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, *token), qsizetype(1));
    const GpuRetireStorageSnapshot storage = GpuRetireRegistry::storageSnapshotForTest();
    QCOMPARE(storage.abandonmentShardVisits, uint64_t(1));
    QCOMPARE(storage.abandonmentNodesVisited, uint64_t(1));
    QCOMPARE(deadSurface.use_count(), deadOwners);
    QVERIFY(liveSurface.use_count() > liveOwners);

    liveFence->setCompleted(liveValue);
    registry.drainCompleted();
    QCOMPARE(liveSurface.use_count(), liveOwners);
    monitor.reset();
}

void TestGpuSurfaceLease::multipleDeadTokensAbandonInOnePass() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    const uint64_t authority = GpuDeviceLossMonitorTestAuthority::capture();
    auto firstSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xD), true,
                                                           GpuSurfaceCompatibility{31, authority});
    auto secondSurface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xE), true,
                                                            GpuSurfaceCompatibility{32, authority});
    const long firstOwners = firstSurface.use_count();
    const long secondOwners = secondSurface.use_count();
    GpuRetireRegistry registry;
    (void) registerLegacyRetire(registry, firstSurface, std::make_shared<FakeFence>(31, authority));
    (void) registerLegacyRetire(registry, secondSurface,
                                std::make_shared<FakeFence>(32, authority));

    GpuDeviceLossMonitorTestAuthority::publish(authority, 31);
    GpuDeviceLossMonitorTestAuthority::publish(authority, 32);
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, monitor.realLossTokens()),
             qsizetype(2));
    QCOMPARE(firstSurface.use_count(), firstOwners);
    QCOMPARE(secondSurface.use_count(), secondOwners);
    monitor.reset();
}

void TestGpuSurfaceLease::registryRegistrationDoesNotPollDriver() {
    GpuRetireRegistry registry;
    const uint64_t authority = currentDeviceAuthority();
    auto fence = std::make_shared<FakeFence>(0xE1, authority);
    std::array<std::shared_ptr<GpuSurface>, 4> surfaces;
    for (quintptr value = 1; value <= 4; ++value)
        surfaces[size_t(value - 1)] = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(value), true, GpuSurfaceCompatibility{0xE1, authority});
    FakeBackendAdapter adapter;
    GpuOpScope operation(fence, registry);
    const auto result = operation.submit(adapter, GpuSurfacePack<4>(surfaces));
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    QCOMPARE(fence->completedCalls(), 0);
    (void) registry.pendingRetainCount();
    QCOMPARE(fence->completedCalls(), 1);
    QVERIFY(fence->completedSawUnlockedRetainer());
    fence->setCompleted(1);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::shardedRegistryQueriesEachFenceOncePerDrain() {
    constexpr size_t fenceCount = 16;
    constexpr size_t recordsPerFence = 4;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    std::array<std::shared_ptr<ConcurrentFence>, fenceCount> fences;
    for (size_t i = 0; i < fenceCount; ++i)
        fences[i] = std::make_shared<ConcurrentFence>(0x300 + i, authorityEpoch);

    for (size_t fenceIndex = 0; fenceIndex < fenceCount; ++fenceIndex) {
        for (size_t record = 0; record < recordsPerFence; ++record) {
            auto surface = std::make_shared<FakeLeaseSurface>(
                reinterpret_cast<void*>(0x3000 + fenceIndex * recordsPerFence + record), true,
                GpuSurfaceCompatibility{0x300 + fenceIndex, authorityEpoch});
            FakeBackendAdapter adapter;
            GpuOpScope operation(fences[fenceIndex], registry);
            const auto result = operation.submit(
                adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
            QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
        }
    }

    for (const auto& fence : fences)
        fence->resetCompletedCalls();
    registry.drainCompleted();
    for (const auto& fence : fences)
        QCOMPARE(fence->completedCalls(), 1);

    for (const auto& fence : fences) {
        fence->advanceToSignalled();
        fence->resetCompletedCalls();
    }
    registry.drainCompleted();
    for (const auto& fence : fences)
        QCOMPARE(fence->completedCalls(), 1);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::allocationProbeCountsRealInjectedHeapEventsByPhase() {
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto runPhase = [&](GpuRetireAllocationPhase phase, uintptr_t handle) {
        const uint64_t authority = currentDeviceAuthority();
        GpuRetireRegistry::resetAllocationProbeForTest();
        GpuRetireRegistry::injectHeapAllocationForNextPhaseForTest(phase);
        auto fence = std::make_shared<FakeFence>(0x72, authority);
        auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(handle), true,
                                                          GpuSurfaceCompatibility{0x72, authority});
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
        const GpuRetireAllocationSnapshot allocations =
            GpuRetireRegistry::allocationSnapshotForTest();
        QCOMPARE(allocations.preparation,
                 phase == GpuRetireAllocationPhase::Preparation ? uint64_t(1) : uint64_t(0));
        QCOMPARE(allocations.callback,
                 phase == GpuRetireAllocationPhase::Callback ? uint64_t(1) : uint64_t(0));
        QCOMPARE(allocations.postAccept,
                 phase == GpuRetireAllocationPhase::PostAccept ? uint64_t(1) : uint64_t(0));
        fence->setCompleted(result.fenceValue);
        registry.drainCompleted();
    };

    runPhase(GpuRetireAllocationPhase::Preparation, 0x721);
    runPhase(GpuRetireAllocationPhase::Callback, 0x722);
    runPhase(GpuRetireAllocationPhase::PostAccept, 0x723);
    GpuRetireRegistry::resetAllocationProbeForTest();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::fullShardDistinctFencesDrainInLinearOperations() {
    constexpr uintptr_t deviceDomain = 0x610;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    const size_t capacity = GpuRetireRegistry::poolCapacityPerShardForTest();
    std::vector<std::shared_ptr<ConcurrentFence>> fences;
    fences.reserve(capacity);
    auto releases = std::make_unique<std::atomic<int>[]>(capacity);
    GpuRetireRegistry::resetStorageProbeForTest();

    for (size_t i = 0; i < capacity; ++i) {
        auto fence = std::make_shared<ConcurrentFence>(deviceDomain, authorityEpoch);
        auto surface = std::make_shared<CountingSurface>(
            reinterpret_cast<void*>(uintptr_t(0x10000 + i)),
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch}, &releases[i]);
        std::atomic<int> callbacks{0};
        ConcurrentBackendAdapter adapter{&callbacks};
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
        fences.push_back(std::move(fence));
    }
    const GpuRetireStorageSnapshot prepareProbe = GpuRetireRegistry::storageSnapshotForTest();

    GpuRetireRegistry::resetStorageProbeForTest();
    registry.drainCompleted();
    const GpuRetireStorageSnapshot incompleteProbe = GpuRetireRegistry::storageSnapshotForTest();
    for (size_t i = 0; i < capacity; ++i)
        QCOMPARE(releases[i].load(std::memory_order_relaxed), 0);

    for (const auto& fence : fences) {
        fence->advanceToSignalled();
        fence->resetCompletedCalls();
    }
    GpuRetireRegistry::resetStorageProbeForTest();
    registry.drainCompleted();
    const GpuRetireStorageSnapshot completedProbe = GpuRetireRegistry::storageSnapshotForTest();
    for (size_t i = 0; i < capacity; ++i) {
        QCOMPARE(fences[i]->completedCalls(), 1);
        QCOMPARE(releases[i].load(std::memory_order_relaxed), 1);
    }
    QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
    for (size_t i = 0; i < capacity; ++i)
        QCOMPARE(releases[i].load(std::memory_order_relaxed), 1);
    QCOMPARE(prepareProbe.poolNodeAcquisitions, uint64_t(capacity));
    QCOMPARE(prepareProbe.poolExhaustions, uint64_t(0));
    QVERIFY2(prepareProbe.fenceLookupSteps <= uint64_t(capacity * 2),
             "fixed hash-bucket preparation must remain linear at full shard capacity");
    QCOMPARE(incompleteProbe.drainShardVisits, uint64_t(1));
    QCOMPARE(incompleteProbe.fenceGroupsVisited, uint64_t(capacity));
    QCOMPARE(incompleteProbe.completionQueries, uint64_t(capacity));
    QCOMPARE(incompleteProbe.activeNodesVisited, uint64_t(capacity));
    QCOMPARE(incompleteProbe.fenceLookupSteps, uint64_t(0));
    QCOMPARE(completedProbe.drainShardVisits, uint64_t(1));
    QCOMPARE(completedProbe.fenceGroupsVisited, uint64_t(capacity));
    QCOMPARE(completedProbe.completionQueries, uint64_t(capacity));
    QCOMPARE(completedProbe.activeNodesVisited, uint64_t(capacity));
    QCOMPARE(completedProbe.fenceLookupSteps, uint64_t(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::boundedWaitRePollsPartialProgressAfterTimeout() {
    constexpr uintptr_t deviceDomain = 0x620;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<PartialProgressFence>(deviceDomain, authorityEpoch);
    std::array<std::atomic<int>, 2> releases{};

    for (size_t i = 0; i < releases.size(); ++i) {
        auto surface = std::make_shared<CountingSurface>(
            reinterpret_cast<void*>(uintptr_t(0x6200 + i)),
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch}, &releases[i]);
        std::atomic<int> callbacks{0};
        ConcurrentBackendAdapter adapter{&callbacks};
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    }

    QCOMPARE(registry.drainWithBoundedWait(10), 1);
    QCOMPARE(fence->waitCalls(), 1);
    QCOMPARE(fence->completedCalls(), 2);
    QCOMPARE(releases[0].load(std::memory_order_relaxed), 1);
    QCOMPARE(releases[1].load(std::memory_order_relaxed), 0);
    QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(1));
    fence->completeTo(2);
    registry.drainCompleted();
    QCOMPARE(releases[0].load(std::memory_order_relaxed), 1);
    QCOMPARE(releases[1].load(std::memory_order_relaxed), 1);
    QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::preparedFenceGroupSurvivesDrainCancelAndRejectsStaleToken() {
    constexpr uintptr_t deviceDomain = 0x2F1;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<FakeFence>(deviceDomain, authorityEpoch);
    auto first =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x2F10), true,
                                           GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
    const std::shared_ptr<GpuSurface> firstOwner = first;
    const long firstOwners = first.use_count();

    {
        auto cancelled = GpuRetireRegistryTestAuthority::prepare(registry, &firstOwner, 1, fence);
        QVERIFY(cancelled);
        const GpuRetirePreparedHandle cancelledHandle =
            GpuRetireRegistryTestAuthority::handle(cancelled);
        registry.drainCompleted();
        QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
        QVERIFY(first.use_count() > firstOwners);
        cancelled = {};
        QCOMPARE(first.use_count(), firstOwners);

        auto second = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(0x2F11), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
        const std::shared_ptr<GpuSurface> secondOwner = second;
        auto prepared = GpuRetireRegistryTestAuthority::prepare(registry, &secondOwner, 1, fence);
        QVERIFY(prepared);
        const auto ticket = fence->submitForTest(fence->identity(), second->compatibility(),
                                                 GpuGenerationCounter::instance().current(),
                                                 []() noexcept { return true; });
        QVERIFY(ticket.has_value());
        QVERIFY(!GpuRetireRegistryTestAuthority::publishHandle(registry, cancelledHandle, *ticket));
        QVERIFY(GpuRetireRegistryTestAuthority::publish(registry, prepared, *ticket));
        QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(1));
        fence->setCompleted(ticket->value());
        registry.drainCompleted();
        QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
    }
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::multiNodePreparedBatchRejectsStaleAbaToken() {
    constexpr uintptr_t deviceDomain = 0x2F4;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<FakeFence>(deviceDomain, authorityEpoch);
    auto makeOwners = [authorityEpoch](uintptr_t base) {
        std::array<std::shared_ptr<GpuSurface>, 6> owners;
        for (size_t i = 0; i < owners.size(); ++i)
            owners[i] = std::make_shared<FakeLeaseSurface>(
                reinterpret_cast<void*>(base + i), true,
                GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
        return owners;
    };

    auto firstOwners = makeOwners(0x2F40);
    auto cancelled =
        GpuRetireRegistryTestAuthority::prepare(registry, firstOwners.data(), 6, fence);
    QVERIFY(cancelled);
    const GpuRetirePreparedHandle stale = GpuRetireRegistryTestAuthority::handle(cancelled);
    QCOMPARE(stale.count, uint16_t(2));
    cancelled = {};

    auto secondOwners = makeOwners(0x2F50);
    auto prepared =
        GpuRetireRegistryTestAuthority::prepare(registry, secondOwners.data(), 6, fence);
    QVERIFY(prepared);
    const auto ticket = fence->submitForTest(fence->identity(), secondOwners[0]->compatibility(),
                                             GpuGenerationCounter::instance().current(),
                                             []() noexcept { return true; });
    QVERIFY(ticket.has_value());
    QVERIFY(!GpuRetireRegistryTestAuthority::publishHandle(registry, stale, *ticket));
    QVERIFY(GpuRetireRegistryTestAuthority::publish(registry, prepared, *ticket));
    QCOMPARE(registry.pendingRetainCount(), qsizetype(6));
    fence->setCompleted(ticket->value());
    registry.drainCompleted();
    QCOMPARE(registry.pendingRetainCount(), qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::duplicateFenceValuesRemainOneExactFenceGroup() {
    constexpr uintptr_t deviceDomain = 0x2F2;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<ConstantValueFence>(deviceDomain, authorityEpoch);
    for (uintptr_t handle : {uintptr_t(0x2F20), uintptr_t(0x2F21)}) {
        auto surface = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(handle), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
        QCOMPARE(result.fenceValue, uint64_t(1));
    }
    fence->complete();
    fence->resetCompletedCalls();
    registry.drainCompleted();
    QCOMPARE(fence->completedCalls(), 1);
    QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::preparedFenceGroupAndResultOwnExactFenceReferences() {
    constexpr uintptr_t deviceDomain = 0x2F3;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<FakeFence>(deviceDomain, authorityEpoch);
    const long ownersBefore = fence.use_count();
    std::array<std::shared_ptr<GpuSurface>, 4> surfaces;
    for (size_t i = 0; i < surfaces.size(); ++i)
        surfaces[i] = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(uintptr_t(0x2F30 + i)), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
    FakeBackendAdapter adapter;
    GpuSubmissionResult result;
    {
        GpuOpScope operation(fence, registry);
        result = operation.submit(adapter, GpuSurfacePack<4>(surfaces));
    }
    const long retainedOwners = fence.use_count();
    QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    fence->setCompleted(result.fenceValue);
    registry.drainCompleted();
    QCOMPARE(retainedOwners, ownersBefore + 2);
    QCOMPARE(fence.use_count(), ownersBefore + 1);
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::diagnosticsSnapshotIsExactAcrossDeterministicShardHandoff() {
    constexpr uintptr_t oldDomain = 0x700; // shard 0
    constexpr uintptr_t newDomain = 0x701; // shard 1
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t authorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;

    auto oldFence = std::make_shared<ZeroSignalFence>(oldDomain, authorityEpoch);
    auto oldSurface = std::make_shared<FakeLeaseSurface>(
        reinterpret_cast<void*>(0x7000), true, GpuSurfaceCompatibility{oldDomain, authorityEpoch});
    FakeBackendAdapter adapter;
    GpuOpScope oldOperation(oldFence, registry);
    const auto oldResult = oldOperation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{oldSurface}));
    QCOMPARE(oldResult.retirement, GpuRetirementDisposition::Quarantined);
    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(authorityEpoch, oldDomain) != 0);
    const std::vector<DeadDeviceToken> deadTokens = monitor.realLossTokens();
    QCOMPARE(deadTokens.size(), size_t(1));
    monitor.reset();
    const uint64_t replacementAuthority = currentDeviceAuthority();

    const GpuRetireDiagnostics before = registry.diagnostics();
    QCOMPARE(before.pendingRetains, qsizetype(1));
    QCOMPARE(before.quarantineCount, qsizetype(1));
    DiagnosticsShardHandoff handoff{
        &registry,
        &deadTokens.front(),
        std::make_shared<ZeroSignalFence>(newDomain, replacementAuthority),
        std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(0x7010), true,
            GpuSurfaceCompatibility{newDomain, replacementAuthority}),
        0,
        {}};
    GpuRetireRegistry::setDiagnosticsHookForTest(&handoffQuarantineBetweenShards, &handoff);
    const GpuRetireDiagnostics during = registry.diagnostics();
    GpuRetireRegistry::setDiagnosticsHookForTest(nullptr, nullptr);

    QCOMPARE(handoff.released, qsizetype(1));
    QCOMPARE(handoff.submitted.retirement, GpuRetirementDisposition::Quarantined);
    QCOMPARE(during.pendingRetains, qsizetype(1));
    QCOMPARE(during.quarantineCount, qsizetype(1));
    const GpuRetireDiagnostics after = registry.diagnostics();
    QCOMPARE(after.pendingRetains, qsizetype(1));
    QCOMPARE(after.quarantineCount, qsizetype(1));
    QVERIFY(before.highWaterMark <= during.highWaterMark);
    QVERIFY(during.highWaterMark <= after.highWaterMark);

    QVERIFY(GpuDeviceLossMonitorTestAuthority::publish(replacementAuthority, newDomain) != 0);
    QCOMPARE(GpuRetireRegistryTestAuthority::abandon(registry, monitor.realLossTokens()),
             qsizetype(1));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::diagnosticsRemainConsistentDuringValidatedAbandon() {
    constexpr uintptr_t firstDeviceDomain = 0x630;
    constexpr int kDomains = 8;
    constexpr int kRecords = 256;
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t authorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    for (int record = 0; record < kRecords; ++record) {
        const uintptr_t deviceDomain = firstDeviceDomain + uintptr_t(record % kDomains);
        auto fence = std::make_shared<FakeFence>(deviceDomain, authorityEpoch);
        auto surface = std::make_shared<FakeLeaseSurface>(
            reinterpret_cast<void*>(uintptr_t(0x6300 + record)), true,
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch});
        FakeBackendAdapter adapter;
        GpuOpScope operation(fence, registry);
        QCOMPARE(operation
                     .submit(adapter,
                             GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}))
                     .retirement,
                 GpuRetirementDisposition::Published);
    }
    QCOMPARE(registry.pendingRetainCount(), qsizetype(kRecords));
    uint64_t lossGeneration = 0;
    for (int domain = 0; domain < kDomains; ++domain) {
        const uint64_t generation = GpuDeviceLossMonitorTestAuthority::publish(
            authorityEpoch, firstDeviceDomain + uintptr_t(domain));
        QVERIFY(generation != 0);
        if (lossGeneration == 0)
            lossGeneration = generation;
        else
            QCOMPARE(generation, lossGeneration);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> abandonDone{false};
    std::atomic<bool> abandonerFailed{false};
    std::atomic<bool> readerFailed{false};
    std::atomic<qsizetype> abandoned{0};

    std::thread abandoner([&]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        const GpuValidatedLossResult recovery =
            monitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains& domains) {
                return registry.abandonAllNoWait(domains);
            });
        if (recovery.status != GpuValidatedLossStatus::Completed)
            abandonerFailed.store(true, std::memory_order_relaxed);
        abandoned.store(recovery.abandoned, std::memory_order_release);
        abandonDone.store(true, std::memory_order_release);
    });
    std::thread reader([&]() {
        start.store(true, std::memory_order_release);
        do {
            const GpuRetireDiagnostics snapshot = registry.diagnostics();
            if (snapshot.highWaterMark < snapshot.pendingRetains ||
                snapshot.quarantineCount > snapshot.pendingRetains)
                readerFailed.store(true, std::memory_order_relaxed);
        } while (!abandonDone.load(std::memory_order_acquire));
    });

    abandoner.join();
    reader.join();
    QVERIFY(!abandonerFailed.load(std::memory_order_relaxed));
    QVERIFY(!readerFailed.load(std::memory_order_relaxed));
    QCOMPARE(abandoned.load(std::memory_order_acquire), qsizetype(kRecords));
    const GpuRetireDiagnostics finalSnapshot = registry.diagnostics();
    QCOMPARE(finalSnapshot.pendingRetains, qsizetype(0));
    QCOMPARE(finalSnapshot.quarantineCount, qsizetype(0));
    QVERIFY(finalSnapshot.highWaterMark >= finalSnapshot.pendingRetains);
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::shardedRegistryConcurrentPublishAndDrainIsExact() {
    constexpr int producerCount = 8;
    constexpr int fenceCount = 16;
    constexpr int recordsPerProducer = 64;
    constexpr int totalRecords = producerCount * recordsPerProducer;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    std::array<std::shared_ptr<ConcurrentFence>, fenceCount> fences;
    for (int i = 0; i < fenceCount; ++i)
        fences[size_t(i)] = std::make_shared<ConcurrentFence>(0x400 + uintptr_t(i), authorityEpoch);

    std::atomic<int> callbacks{0};
    std::atomic<int> published{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> producersDone{0};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            for (int record = 0; record < recordsPerProducer; ++record) {
                const int fenceIndex = (producer * recordsPerProducer + record) % fenceCount;
                const uintptr_t domain = 0x400 + uintptr_t(fenceIndex);
                auto surface = std::make_shared<CountingSurface>(
                    reinterpret_cast<void*>(
                        uintptr_t(0x4000 + producer * recordsPerProducer + record)),
                    GpuSurfaceCompatibility{domain, authorityEpoch}, &destroyed);
                ConcurrentBackendAdapter adapter{&callbacks};
                GpuOpScope operation(fences[size_t(fenceIndex)], registry);
                const auto result = operation.submit(
                    adapter,
                    GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
                if (result.retirement == GpuRetirementDisposition::Published)
                    published.fetch_add(1, std::memory_order_relaxed);
            }
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    std::thread drainer([&]() {
        do {
            for (const auto& fence : fences)
                fence->advanceToSignalled();
            registry.drainCompleted();
            std::this_thread::yield();
        } while (producersDone.load(std::memory_order_acquire) != producerCount ||
                 registry.diagnostics().pendingRetains != 0);
    });

    for (auto& producer : producers)
        producer.join();
    drainer.join();
    for (const auto& fence : fences)
        fence->advanceToSignalled();
    registry.drainCompleted();

    QCOMPARE(callbacks.load(std::memory_order_relaxed), totalRecords);
    QCOMPARE(published.load(std::memory_order_relaxed), totalRecords);
    QCOMPARE(destroyed.load(std::memory_order_relaxed), totalRecords);
    QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::preparedPoolExhaustionRejectsBeforeDriverAcceptance() {
    constexpr uintptr_t deviceDomain = 0x510;
    const uint64_t authorityEpoch = currentDeviceAuthority();
    GpuGenerationCounter::instance().resetForTest();
    GpuRetireRegistry registry;
    auto fence = std::make_shared<ConcurrentFence>(deviceDomain, authorityEpoch);
    std::atomic<int> callbacks{0};
    std::atomic<int> destroyed{0};
    const size_t capacity = GpuRetireRegistry::poolCapacityPerShardForTest();
    QVERIFY(capacity > 0);
    GpuRetireRegistry::resetStorageProbeForTest();

    for (size_t i = 0; i < capacity; ++i) {
        auto surface = std::make_shared<CountingSurface>(
            reinterpret_cast<void*>(0x5100 + i),
            GpuSurfaceCompatibility{deviceDomain, authorityEpoch}, &destroyed);
        ConcurrentBackendAdapter adapter{&callbacks};
        GpuOpScope operation(fence, registry);
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        QCOMPARE(result.retirement, GpuRetirementDisposition::Published);
    }

    auto overflowSurface = std::make_shared<CountingSurface>(
        reinterpret_cast<void*>(0x5FFF), GpuSurfaceCompatibility{deviceDomain, authorityEpoch},
        &destroyed);
    ConcurrentBackendAdapter overflowAdapter{&callbacks};
    GpuOpScope overflowOperation(fence, registry);
    const auto overflowResult = overflowOperation.submit(
        overflowAdapter,
        GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{overflowSurface}));
    QCOMPARE(overflowResult.outcome, GpuSubmitOutcome::NotSubmitted);
    QCOMPARE(overflowResult.retirement, GpuRetirementDisposition::None);
    QCOMPARE(callbacks.load(std::memory_order_relaxed), int(capacity));
    QCOMPARE(GpuRetireRegistry::storageSnapshotForTest().poolExhaustions, uint64_t(1));

    overflowSurface.reset();
    fence->advanceToSignalled();
    registry.drainCompleted();
    QCOMPARE(destroyed.load(std::memory_order_relaxed), int(capacity + 1));
    QCOMPARE(registry.diagnostics().pendingRetains, qsizetype(0));
    GpuGenerationCounter::instance().resetForTest();
}

void TestGpuSurfaceLease::multiNodePoolExhaustionIsAllOrNothing() {
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t authority = currentDeviceAuthority();
    verifyMultiNodePoolExhaustionIsAtomic<6>(0x521, authority);
    verifyMultiNodePoolExhaustionIsAtomic<11>(0x522, authority);
    verifyMultiNodePoolExhaustionIsAtomic<16>(0x523, authority);
    GpuGenerationCounter::instance().resetForTest();
}

QTEST_GUILESS_MAIN(TestGpuSurfaceLease)
#include "tst_gpusurfacelease.moc"
