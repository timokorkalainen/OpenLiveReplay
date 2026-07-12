#include "playback/gpu/gpuretireregistry.h"

#include "playback/gpu/gpureadbackretainer.h"

#include <utility>

void GpuRetireRegistry::registerRetire(std::shared_ptr<GpuSurface> surface,
                                       std::shared_ptr<GpuFence> fence, uint64_t fenceValue) const {
    gpuRetireDetail::registerRetire(std::move(surface), std::move(fence), fenceValue);
}

void GpuRetireRegistry::drainCompleted() const {
    gpuRetireDetail::drainCompleted();
}

qsizetype GpuRetireRegistry::pendingRetainCount() const {
    return gpuRetireDetail::pendingCount();
}

qsizetype GpuRetireRegistry::abandonAllNoWait(const DeadDeviceToken& deadDevice) const {
    return gpuRetireDetail::abandonAllNoWait(deadDevice);
}

int GpuRetireRegistry::drainWithBoundedWait(int perFenceTimeoutMs) const {
    return gpuRetireDetail::drainWithBoundedWait(perFenceTimeoutMs);
}

GpuRetireDiagnostics GpuRetireRegistry::diagnostics() const {
    return GpuRetireDiagnostics{gpuRetireDetail::pendingCount(), gpuRetireDetail::highWaterMark(),
                                gpuRetireDetail::timeoutCount(),
                                gpuRetireDetail::signalFailureCount()};
}

void GpuRetireRegistry::registerRetireBatch(std::shared_ptr<GpuSurface>* surfaces, qsizetype count,
                                            const std::shared_ptr<GpuFence>& fence,
                                            uint64_t fenceValue) const {
    gpuRetireDetail::registerRetireBatch(surfaces, count, fence, fenceValue);
}

void GpuRetireRegistry::noteSignalFailure() const {
    gpuRetireDetail::noteSignalFailure();
}
