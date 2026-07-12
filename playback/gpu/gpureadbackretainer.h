#ifndef OLR_GPU_READBACK_RETAINER_H
#define OLR_GPU_READBACK_RETAINER_H

#include <QtGlobal>

#include <cstdint>
#include <memory>

class DeadDeviceToken;
class GpuFence;
class GpuSurface;

// Storage implementation for GpuRetireRegistry. Production call sites use the
// facade; this namespace exists only to keep the existing single global store.
namespace gpuRetireDetail {

void registerRetire(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuFence> fence,
                    uint64_t fenceValue);
void drainCompleted();
qsizetype pendingCount();
qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice);
int drainWithBoundedWait(int perFenceTimeoutMs);
qsizetype highWaterMark();
uint64_t timeoutCount();
uint64_t signalFailureCount();
void noteSignalFailure();

#ifdef OLR_UNIT_TEST
bool mutexAvailableForTest();
#endif

} // namespace gpuRetireDetail

#endif // OLR_GPU_READBACK_RETAINER_H
