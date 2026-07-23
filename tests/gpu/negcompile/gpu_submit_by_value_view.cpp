#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"

#include <array>
#include <memory>

struct ForbiddenByValueViewAdapter {
    GpuSubmitOutcome operator()(GpuScopedNativeView<1>) noexcept {
        return GpuSubmitOutcome::Submitted;
    }
};

GpuSubmissionResult forbiddenByValueViewSubmission(std::shared_ptr<GpuFence> fence,
                                                   std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    ForbiddenByValueViewAdapter adapter;
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}
