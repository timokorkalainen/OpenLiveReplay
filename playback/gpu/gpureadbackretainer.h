#ifndef OLR_GPU_READBACK_RETAINER_H
#define OLR_GPU_READBACK_RETAINER_H

#include <QtGlobal>

#include <cstdint>
#include <memory>

class DeadDeviceToken;
class GpuFence;
class GpuSurface;

void gpuRetainSurfaceUntilFenceRetired(std::shared_ptr<GpuSurface> surface,
                                       std::shared_ptr<GpuFence> fence, uint64_t fenceValue);
void gpuDrainCompletedReadbackRetains();
qsizetype gpuPendingReadbackRetainCount();

// Device-loss recovery. Drops every held surface WITHOUT waiting on its fence — safe
// only once the device is known dead, because those fences may never advance. The
// DeadDeviceToken is the type-level proof of that: it is mintable only at the two
// driver-authoritative detection sites, so this no-wait free can never run on a live
// device (there is deliberately no token-free overload). Returns retains dropped.
qsizetype gpuAbandonAllReadbackRetains(const DeadDeviceToken& deadDevice);

// Bounded per-entry fence wait then release, for the injected-loss test path where
// the device is still live and its fences DO advance. Returns entries released.
int gpuDrainReadbackRetainsWithBoundedWait(int perFenceTimeoutMs);

#endif // OLR_GPU_READBACK_RETAINER_H
