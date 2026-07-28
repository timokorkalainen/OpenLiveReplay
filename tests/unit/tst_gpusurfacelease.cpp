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

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/framepixelformat.h"

#include <memory>
#include <limits>
#include <type_traits>

#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority {
    static uint64_t capture() {
        return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    }
    static uint64_t publish(uint64_t deviceAuthorityEpoch = capture()) {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch);
    }
};
#endif

namespace {

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
static_assert(!std::is_copy_constructible<GpuReadLease>::value,
              "GpuReadLease must not escape a synchronous callback by copy.");
static_assert(!std::is_move_constructible<GpuReadLease>::value,
              "GpuReadLease must not escape a synchronous callback by move.");

// A surface whose native handle is a known sentinel. nativeHandle() is protected,
// mirroring the production surfaces, so the ONLY way the test reads it is via a lease.
class FakeLeaseSurface : public GpuSurface {
public:
    FakeLeaseSurface(void* handle, bool valid) : m_handle(handle), m_valid(valid) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16, 0}; }
    bool isValid() const override { return m_valid; }

protected:
    void* nativeHandle() const override { return m_valid ? m_handle : nullptr; }

private:
    void* m_handle = nullptr;
    bool m_valid = false;
};

// Fence with a test-controllable completed watermark.
class FakeFence : public GpuFence {
public:
    uint64_t signal() override {
        ++m_signalCalls;
        return ++m_signalled;
    }
    bool wait(uint64_t value, int /*timeoutMs*/) override {
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
    int completedCalls() const { return m_completedCalls; }
    bool completedSawUnlockedRetainer() const { return m_completedSawUnlockedRetainer; }

private:
    uint64_t m_signalled = 0;
    uint64_t m_completed = 0;
    bool m_retireOnWait = false;
    bool m_waitSawUnlockedRetainer = false;
    int m_signalCalls = 0;
    mutable int m_completedCalls = 0;
    mutable bool m_completedSawUnlockedRetainer = false;
};

class ZeroSignalFence final : public GpuFence {
public:
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

} // namespace

class TestGpuSurfaceLease : public QObject {
    Q_OBJECT
private slots:
    void callbackLeaseExposesMetadataOnly();
    void callbackLeaseReportsInvalidSurface();
    void boundedWaitDrainReleasesOnlyRetired();
    void boundedWaitDoesNotHoldRetainerMutex();
    void registryDiagnosticsTrackHighWaterAndTimeouts();
    void registryRegistrationDoesNotPollDriver();
    void opScopeSignalsOnceAndRegistersUniqueSurfaces();
    void opScopeCancelsBeforeSubmissionWithoutRetaining();
    void opScopeRetainsAfterSubmittedError();
    void opScopeQuarantinesOnZeroSignal();
    void zeroSignalQuarantineReleasesAfterAuthoritativeUpgrade();
};

void TestGpuSurfaceLease::callbackLeaseExposesMetadataOnly() {
    auto sentinel = reinterpret_cast<void*>(0xBEEF);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, /*valid=*/true);
    {
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        const auto state = std::make_pair(lease.valid(), lease.desc().width);
        QVERIFY(state.first);
        QCOMPARE(state.second, 16);
    }
}

void TestGpuSurfaceLease::callbackLeaseReportsInvalidSurface() {
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x1), /*valid=*/false);
    GpuSyncReadScope scope;
    const bool valid = scope.read(surface).valid();
    QVERIFY(!valid);
}

void TestGpuSurfaceLease::boundedWaitDrainReleasesOnlyRetired() {
    // Track our own surface's shared ownership rather than the global count, so other
    // tests' retains cannot perturb the assertions.
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x2), /*valid=*/true);
    auto fence = std::make_shared<FakeFence>();
    const long baseline = surface.use_count();

    GpuRetireRegistry registry;
    registry.registerRetire(surface, fence, 5);
    QVERIFY(surface.use_count() > baseline); // the retainer holds a reference

    // Fence has not reached 5 -> bounded wait cannot retire it -> surface stays held.
    registry.drainWithBoundedWait(1);
    QVERIFY(surface.use_count() > baseline);

    // Fence retires -> the bounded-wait drain releases the surface.
    fence->setCompleted(5);
    registry.drainWithBoundedWait(1);
    QCOMPARE(surface.use_count(), baseline);
}

void TestGpuSurfaceLease::boundedWaitDoesNotHoldRetainerMutex() {
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x3), true);
    auto fence = std::make_shared<FakeFence>();
    fence->setRetireOnWait(true);
    GpuRetireRegistry registry;
    registry.registerRetire(surface, fence, 9);

    QCOMPARE(registry.drainWithBoundedWait(1), 1);
    QVERIFY(fence->waitSawUnlockedRetainer());
}

void TestGpuSurfaceLease::registryDiagnosticsTrackHighWaterAndTimeouts() {
    GpuRetireRegistry registry;
    const GpuRetireDiagnostics before = registry.diagnostics();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x4), true);
    auto fence = std::make_shared<FakeFence>();
    registry.registerRetire(surface, fence, 11);

    const GpuRetireDiagnostics registered = registry.diagnostics();
    QVERIFY(registered.pendingRetains >= 1);
    QVERIFY(registered.highWaterMark >= registered.pendingRetains);
    QCOMPARE(registry.drainWithBoundedWait(1), 0);
    QVERIFY(registry.diagnostics().timeoutCount >= before.timeoutCount + 1);

    fence->setCompleted(11);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::opScopeSignalsOnceAndRegistersUniqueSurfaces() {
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t spillsBefore = GpuOpScope::spillAllocationCount();
    auto first = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x5), true);
    auto second = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x6), true);
    auto fence = std::make_shared<FakeFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(first));
    QVERIFY(!operation.track(first));
    QVERIFY(operation.track(second));
    QVERIFY(operation.submit([] { return GpuSubmitOutcome::Submitted; }));

    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 2);
    QCOMPARE(GpuOpScope::spillAllocationCount(), spillsBefore);
    fence->setCompleted(1);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::opScopeCancelsBeforeSubmissionWithoutRetaining() {
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x7), true);
    const long ownersBefore = surface.use_count();
    auto fence = std::make_shared<FakeFence>();

    {
        GpuOpScope operation(fence, registry);
        QVERIFY(operation.track(surface));
        QVERIFY(!operation.submit([] { return GpuSubmitOutcome::NotSubmitted; }));
    }

    QCOMPARE(fence->signalCalls(), 0);
    QCOMPARE(surface.use_count(), ownersBefore);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
}

void TestGpuSurfaceLease::opScopeQuarantinesOnZeroSignal() {
    GpuDeviceLossMonitor::instance().reset();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    const uint64_t failuresBefore = registry.diagnostics().signalFailureCount;
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x8), true);
    auto fence = std::make_shared<ZeroSignalFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(surface));
    QVERIFY(!operation.submit([] { return GpuSubmitOutcome::Submitted; }));
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QCOMPARE(registry.diagnostics().signalFailureCount, failuresBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(!GpuDeviceLossMonitor::instance().realLossToken().has_value());

    fence->retireQuarantine();
    registry.drainCompleted();
    GpuDeviceLossMonitor::instance().reset();
}

void TestGpuSurfaceLease::zeroSignalQuarantineReleasesAfterAuthoritativeUpgrade() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();
    const uint64_t deviceAuthorityEpoch = GpuDeviceLossMonitorTestAuthority::capture();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0xA), true);
    const long ownersBefore = surface.use_count();
    auto fence = std::make_shared<ZeroSignalFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(surface));
    QVERIFY(!operation.submit([] { return GpuSubmitOutcome::Submitted; }));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(surface.use_count() > ownersBefore);

    const uint64_t lossGeneration =
        GpuDeviceLossMonitorTestAuthority::publish(deviceAuthorityEpoch);
    QVERIFY(lossGeneration != 0);
    const auto token = monitor.realLossToken();
    QVERIFY(token.has_value());
    QCOMPARE(registry.abandonAllNoWait(*token), qsizetype(1));
    QCOMPARE(registry.pendingRetainCount(), pendingBefore);
    QCOMPARE(surface.use_count(), ownersBefore);
    monitor.reset();
}

void TestGpuSurfaceLease::registryRegistrationDoesNotPollDriver() {
    GpuRetireRegistry registry;
    auto fence = std::make_shared<FakeFence>();
    GpuOpScope operation(fence, registry);
    for (quintptr value = 1; value <= 4; ++value)
        QVERIFY(operation.track(
            std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(value), true)));
    QVERIFY(operation.submit([] { return GpuSubmitOutcome::Submitted; }));
    QCOMPARE(fence->completedCalls(), 1);
    QVERIFY(fence->completedSawUnlockedRetainer());
    fence->setCompleted(1);
    registry.drainCompleted();
}

void TestGpuSurfaceLease::opScopeRetainsAfterSubmittedError() {
    GpuDeviceLossMonitor::instance().reset();
    GpuRetireRegistry registry;
    const qsizetype pendingBefore = registry.pendingRetainCount();
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x9), true);
    auto fence = std::make_shared<FakeFence>();

    GpuOpScope operation(fence, registry);
    QVERIFY(operation.track(surface));
    QVERIFY(!operation.submit([] { return GpuSubmitOutcome::SubmittedWithError; }));
    QCOMPARE(fence->signalCalls(), 1);
    QCOMPARE(registry.pendingRetainCount(), pendingBefore + 1);
    QVERIFY(GpuDeviceLossMonitor::instance().isLost());
    QVERIFY(!GpuDeviceLossMonitor::instance().realLossToken().has_value());

    fence->setCompleted(1);
    registry.drainCompleted();
    GpuDeviceLossMonitor::instance().reset();
}

QTEST_GUILESS_MAIN(TestGpuSurfaceLease)
#include "tst_gpusurfacelease.moc"
