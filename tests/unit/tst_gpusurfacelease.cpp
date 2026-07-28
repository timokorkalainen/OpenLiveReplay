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
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/framepixelformat.h"

#include <memory>
#include <type_traits>

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

// A surface whose native handle is a known sentinel. nativeHandle() is protected,
// mirroring the production surfaces, so the ONLY way the test reads it is via a lease.
class FakeLeaseSurface : public GpuSurface {
public:
    FakeLeaseSurface(void* handle, bool valid) : m_handle(handle), m_valid(valid) {}
    GpuSurfaceDesc desc() const override { return {FramePixelFormat::Nv12, 16, 16, 0}; }
    bool isValid() const override { return m_valid; }
    void* expectedHandleForTest() const { return m_valid ? m_handle : nullptr; }

protected:
    void* nativeHandle() const override { return m_valid ? m_handle : nullptr; }

private:
    void* m_handle = nullptr;
    bool m_valid = false;
};

// Fence with a test-controllable completed watermark.
class FakeFence : public GpuFence {
public:
    uint64_t signal() override { return ++m_signalled; }
    bool wait(uint64_t value, int /*timeoutMs*/) override { return m_completed >= value; }
    uint64_t completedValue() const override { return m_completed; }
    void setCompleted(uint64_t v) { m_completed = v; }

private:
    uint64_t m_signalled = 0;
    uint64_t m_completed = 0;
};

} // namespace

class TestGpuSurfaceLease : public QObject {
    Q_OBJECT
private slots:
    void leaseHandsOutTheRealHandle();
    void leaseOnInvalidSurfaceIsNull();
    void boundedWaitDrainReleasesOnlyRetired();
};

void TestGpuSurfaceLease::leaseHandsOutTheRealHandle() {
    auto sentinel = reinterpret_cast<void*>(0xBEEF);
    auto surface = std::make_shared<FakeLeaseSurface>(sentinel, /*valid=*/true);
    {
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        QVERIFY(lease.valid());
        QCOMPARE(lease.nativeHandle(), surface->expectedHandleForTest());
        QCOMPARE(lease.nativeHandle(), sentinel);
        QCOMPARE(lease.desc().width, 16);
        scope.complete(); // synchronous read finished; destructor must not assert
    }
}

void TestGpuSurfaceLease::leaseOnInvalidSurfaceIsNull() {
    auto surface =
        std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x1), /*valid=*/false);
    GpuSyncReadScope scope;
    const GpuReadLease lease = scope.read(surface);
    QVERIFY(!lease.valid());
    QCOMPARE(lease.nativeHandle(), nullptr);
    scope.complete();
}

void TestGpuSurfaceLease::boundedWaitDrainReleasesOnlyRetired() {
    // Track our own surface's shared ownership rather than the global count, so other
    // tests' retains cannot perturb the assertions.
    auto surface = std::make_shared<FakeLeaseSurface>(reinterpret_cast<void*>(0x2), /*valid=*/true);
    auto fence = std::make_shared<FakeFence>();
    const long baseline = surface.use_count();

    gpuRetainSurfaceUntilFenceRetired(surface, fence, 5);
    QVERIFY(surface.use_count() > baseline); // the retainer holds a reference

    // Fence has not reached 5 -> bounded wait cannot retire it -> surface stays held.
    gpuDrainReadbackRetainsWithBoundedWait(1);
    QVERIFY(surface.use_count() > baseline);

    // Fence retires -> the bounded-wait drain releases the surface.
    fence->setCompleted(5);
    gpuDrainReadbackRetainsWithBoundedWait(1);
    QCOMPARE(surface.use_count(), baseline);
}

QTEST_GUILESS_MAIN(TestGpuSurfaceLease)
#include "tst_gpusurfacelease.moc"
