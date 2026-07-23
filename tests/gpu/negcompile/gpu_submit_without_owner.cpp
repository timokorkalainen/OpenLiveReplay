#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"

#include <array>
#include <memory>

struct OwnerlessSubmissionAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::NotSubmitted; }
};

GpuSubmissionResult forbiddenOwnerlessSubmission(std::shared_ptr<GpuFence> fence) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    OwnerlessSubmissionAdapter adapter;
    return operation.submit(adapter);
}
