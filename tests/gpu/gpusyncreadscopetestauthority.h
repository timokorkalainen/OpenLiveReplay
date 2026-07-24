#ifndef OLR_TESTS_GPUSYNCREADSCOPETESTAUTHORITY_H
#define OLR_TESTS_GPUSYNCREADSCOPETESTAUTHORITY_H

#include "playback/gpu/gpusurfacelease.h"

#include <memory>

#ifndef OLR_UNIT_TEST
#error "GpuSyncReadScopeTestAuthority is a test-only capability"
#endif

// Test-only access to the private acquire/discharge halves of GpuSyncReadScope.
//
// Production code has no route to them: withRead() is the only public entry point,
// so an op site cannot obtain a native handle without a scope whose exit discharges
// it. Tests that must observe the halves separately — repeated acquisition, a lease
// escaped past its scope, access after complete() — go through this authority, which
// exists only in translation units compiled with OLR_UNIT_TEST.
struct GpuSyncReadScopeTestAuthority {
    template <typename Surface>
    static GpuReadLease read(GpuSyncReadScope& scope, const std::shared_ptr<Surface>& surface) {
        return scope.read(surface);
    }

    static GpuReadLease read(GpuSyncReadScope& scope, GpuSurface* surface) {
        return scope.read(surface);
    }

    static void complete(GpuSyncReadScope& scope) noexcept { scope.complete(); }
};

#endif // OLR_TESTS_GPUSYNCREADSCOPETESTAUTHORITY_H
