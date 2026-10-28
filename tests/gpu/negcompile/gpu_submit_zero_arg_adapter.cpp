#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"

#include <array>
#include <memory>

struct ForbiddenZeroArgAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::Submitted; }
};

GpuSubmissionResult forbiddenZeroArgSubmission(std::shared_ptr<GpuFence> fence,
                                               std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    ForbiddenZeroArgAdapter adapter;
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}
