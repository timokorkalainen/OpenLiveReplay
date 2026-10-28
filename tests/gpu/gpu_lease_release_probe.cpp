#include "tests/gpu/gpusyncreadscopetestauthority.h"

#include "playback/gpu/gpusurfacelease.h"

#include <memory>
#include <type_traits>

namespace {

class ProbeSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return {}; }

protected:
    void* nativeHandle() const override { return reinterpret_cast<void*>(0x51); }
};

GpuReadLease escapeLeaseByGuaranteedElision(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    return GpuSyncReadScopeTestAuthority::read(scope, surface);
}

} // namespace

static_assert(!std::is_move_constructible_v<GpuReadLease>);
static_assert(!std::is_move_assignable_v<GpuReadLease>);
static_assert(!std::is_move_constructible_v<GpuSyncReadScope>);
static_assert(!std::is_move_assignable_v<GpuSyncReadScope>);

int main() {
    auto surface = std::make_shared<ProbeSurface>();

    const GpuReadLease escaped = escapeLeaseByGuaranteedElision(surface);
    if (!escaped.valid() || escaped.nativeHandle() != nullptr) return 1;

    GpuSyncReadScope repeatedScope;
    const GpuReadLease first = GpuSyncReadScopeTestAuthority::read(repeatedScope, surface);
    const GpuReadLease repeated = GpuSyncReadScopeTestAuthority::read(repeatedScope, surface);
    if (repeated.valid() || first.nativeHandle() != nullptr || repeated.nativeHandle() != nullptr) {
        return 2;
    }

    GpuSyncReadScope deregistrationScope;
    {
        const GpuReadLease lease =
            GpuSyncReadScopeTestAuthority::read(deregistrationScope, surface);
        if (lease.nativeHandle() == nullptr) return 3;
    }
    GpuSyncReadScopeTestAuthority::complete(deregistrationScope);
    return 0;
}
