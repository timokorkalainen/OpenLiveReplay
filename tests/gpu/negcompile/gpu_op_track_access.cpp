#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"

#include <memory>

int forbiddenGpuTrackAccess(std::shared_ptr<GpuFence> fence, std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    return operation.track(std::move(surface)) ? 0 : 1;
}
