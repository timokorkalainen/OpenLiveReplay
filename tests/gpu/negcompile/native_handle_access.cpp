#include "playback/gpu/gpusurface.h"

class DerivedSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {}; }
    bool isValid() const override { return true; }

protected:
    void* nativeHandle() const override { return nullptr; }
};

void forbiddenNativeHandleAccess(const GpuSurface& surface) {
    (void) surface.nativeHandle();
}

void forbiddenDerivedNativeHandleAccess(const DerivedSurface& surface) {
    (void) surface.nativeHandle();
}
