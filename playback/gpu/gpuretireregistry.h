#ifndef OLR_GPU_RETIRE_REGISTRY_H
#define OLR_GPU_RETIRE_REGISTRY_H

#include <QtGlobal>

#include <cstdint>
#include <memory>

class DeadDeviceToken;
class GpuFence;
class GpuOpScope;
class GpuSurface;

struct GpuRetireDiagnostics {
    qsizetype pendingRetains = 0;
    qsizetype highWaterMark = 0;
    uint64_t timeoutCount = 0;
    uint64_t signalFailureCount = 0;
};

class GpuRetireRegistry final {
public:
    void registerRetire(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuFence> fence,
                        uint64_t fenceValue) const;
    void drainCompleted() const;
    qsizetype pendingRetainCount() const;
    qsizetype abandonAllNoWait(const DeadDeviceToken& deadDevice) const;
    int drainWithBoundedWait(int perFenceTimeoutMs) const;
    GpuRetireDiagnostics diagnostics() const;

private:
    friend class GpuOpScope;
    void registerRetireBatch(std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                             const std::shared_ptr<GpuFence>& fence, uint64_t fenceValue) const;
    void noteSignalFailure() const;
};

#endif // OLR_GPU_RETIRE_REGISTRY_H
