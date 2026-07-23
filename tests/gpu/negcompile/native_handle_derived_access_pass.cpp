#include "playback/gpu/gpusurfacelease.h"

#include <memory>

class DerivedSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {}; }
    bool isValid() const override { return true; }

protected:
    void* nativeHandle() const override { return reinterpret_cast<void*>(0x1); }
};

bool allowedDerivedNativeHandleAccess(const std::shared_ptr<DerivedSurface>& surface) {
    GpuSyncReadScope scope;
    const GpuReadLease lease = scope.read(surface);
    void* handle = lease.nativeHandle();
    scope.complete();
    return handle != nullptr;
}
