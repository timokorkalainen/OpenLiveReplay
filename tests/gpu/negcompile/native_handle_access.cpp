#include "playback/gpu/gpusurface.h"

void forbiddenNativeHandleAccess(const GpuSurface& surface) {
    (void) surface.nativeHandle();
}
