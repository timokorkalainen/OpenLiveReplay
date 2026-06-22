#ifndef OLR_GPU_READBACK_RETAINER_H
#define OLR_GPU_READBACK_RETAINER_H

#include <QtGlobal>

#include <cstdint>
#include <memory>

class GpuFence;
class GpuSurface;

void gpuRetainSurfaceUntilFenceRetired(std::shared_ptr<GpuSurface> surface,
                                       std::shared_ptr<GpuFence> fence, uint64_t fenceValue);
void gpuDrainCompletedReadbackRetains();
qsizetype gpuPendingReadbackRetainCount();

#endif // OLR_GPU_READBACK_RETAINER_H
