#include "playback/gpu/gpusurfacelease.h"

#include <memory>

bool allowedNativeHandleAccess(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    const GpuReadLease lease = scope.read(surface);
    void* handle = lease.nativeHandle();
    scope.complete();
    return handle != nullptr;
}
