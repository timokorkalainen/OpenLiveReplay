#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"

#include <array>
#include <memory>

struct StubSubmissionAdapter {
    GpuSubmitOutcome operator()(const GpuScopedNativeView<1>& view) noexcept {
        (void) view[0].nativeHandle();
        return GpuSubmitOutcome::NotSubmitted;
    }
};

GpuSubmissionResult stubTypedSubmissionPass(std::shared_ptr<GpuFence> fence,
                                            std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    StubSubmissionAdapter adapter;
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}
