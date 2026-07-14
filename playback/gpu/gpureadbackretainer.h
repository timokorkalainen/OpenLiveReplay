#ifndef OLR_GPU_READBACK_RETAINER_H
#define OLR_GPU_READBACK_RETAINER_H

#include <QtGlobal>

#include <cstdint>
#include <memory>
#include <vector>

class DeadDeviceToken;
class GpuFence;
class GpuRetireRegistry;
class GpuRetirementTicket;
class GpuSurface;

// Private legacy-store implementation. Only the registry facade can publish an
// immutable fence-minted ticket; arbitrary surface/fence/value pairing is not a
// callable capability outside that authority boundary.
class GpuReadbackRetainer final {
private:
    friend class GpuRetireRegistry;

    static void registerRetire(std::shared_ptr<GpuSurface> surface, GpuRetirementTicket ticket);
    static void drainCompleted();
    static qsizetype pendingCount();
    static qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice);
    static qsizetype abandonAllNoWait(const std::vector<DeadDeviceToken>& deadDevices);
    static int drainWithBoundedWait(int totalTimeoutMs);
    static qsizetype highWaterMark();
    static uint64_t timeoutCount();
    static uint64_t signalFailureCount();
    static void noteSignalFailure();
};

#ifdef OLR_UNIT_TEST
namespace gpuRetireDetail {
bool mutexAvailableForTest();
} // namespace gpuRetireDetail
#endif

#endif // OLR_GPU_READBACK_RETAINER_H
