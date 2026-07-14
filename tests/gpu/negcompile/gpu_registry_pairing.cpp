#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpusurface.h"

#include <memory>

void forbiddenArbitraryRegistryPairing(std::shared_ptr<GpuSurface> surface,
                                       std::shared_ptr<GpuFence> fence) {
    gpuRetireDetail::registerRetire(std::move(surface), std::move(fence), 0xBADu);
}
