#include "playback/gpu/gpusurfacelease.h"

#include <memory>

struct NativeHandleWrapper {
    explicit NativeHandleWrapper(void* value) : handle(value) {}
    void* handle = nullptr;
};

NativeHandleWrapper forbiddenWithReadWrapperReturn(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    return scope.withRead(surface, [](const GpuReadLease& lease) {
        return NativeHandleWrapper(lease.nativeHandle());
    });
}
