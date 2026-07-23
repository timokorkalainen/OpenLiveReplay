#include "playback/gpu/gpusurfacelease.h"

#include <cstdint>
#include <memory>

uintptr_t forbiddenWithReadIntegerReturn(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    return scope.withRead(surface, [](const GpuReadLease& lease) {
        return reinterpret_cast<uintptr_t>(lease.nativeHandle());
    });
}
