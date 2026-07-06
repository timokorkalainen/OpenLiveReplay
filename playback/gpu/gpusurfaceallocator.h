#ifndef OLR_GPUSURFACEALLOCATOR_H
#define OLR_GPUSURFACEALLOCATOR_H

#include "playback/gpu/gpubudget.h"
#include "playback/output/framehandle.h"

#include <functional>
#include <memory>

class GpuFence;
class GpuRhiContext;
class GpuSurface;

struct GpuMintResult {
    FrameHandle handle;
    bool degradedToCpu = false;
};

using GpuFrameHandleFactory =
    std::function<FrameHandle(std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge)>;

GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface, FrameMetadata meta,
                               const GpuFrameHandleFactory& gpuFactory,
                               const std::function<CpuPlanes()>& cpuFallback);
GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence,
                               const std::function<CpuPlanes()>& cpuFallback);

#endif // OLR_GPUSURFACEALLOCATOR_H
