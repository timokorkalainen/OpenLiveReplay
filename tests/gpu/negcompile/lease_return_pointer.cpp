#include "playback/gpu/gpusurfacelease.h"

const GpuReadLease* forbiddenLeasePointer(GpuSurface* surface) {
    GpuSyncReadScope scope;
    return scope.read(surface, [](const GpuReadLease& lease) { return &lease; });
}
