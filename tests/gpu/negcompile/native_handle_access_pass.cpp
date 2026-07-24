#include "playback/gpu/gpusurfacelease.h"

#include <memory>

bool allowedNativeHandleAccess(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    void* handle = nullptr;
    scope.withRead(surface,
                   [&handle](const GpuReadLease& lease) { handle = lease.nativeHandle(); });
    return handle != nullptr;
}
