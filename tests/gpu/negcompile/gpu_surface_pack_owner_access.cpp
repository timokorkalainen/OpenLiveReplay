#include "playback/gpu/gpuopscope.h"

#include <array>
#include <memory>

const std::shared_ptr<GpuSurface>& forbiddenSurfacePackOwnerAccess(const GpuSurfacePack<1>& pack) {
    return pack.owners()[0];
}
