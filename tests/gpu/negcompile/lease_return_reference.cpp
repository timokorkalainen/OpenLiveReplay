#include "playback/gpu/gpusurfacelease.h"

const GpuReadLease& forbiddenLeaseReference(GpuSurface* surface) {
    GpuSyncReadScope scope;
    return scope.read(surface,
                      [](const GpuReadLease& lease) -> const GpuReadLease& { return lease; });
}
