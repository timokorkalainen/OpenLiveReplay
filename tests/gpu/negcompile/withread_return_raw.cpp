#include "playback/gpu/gpusurfacelease.h"

#include <memory>

void* forbiddenWithReadRawReturn(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    return scope.withRead(surface, [](const GpuReadLease& lease) { return lease.nativeHandle(); });
}
