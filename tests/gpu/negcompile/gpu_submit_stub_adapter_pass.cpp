#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"

#include <array>
#include <memory>

struct StubSubmissionAdapter {
    std::shared_ptr<GpuSurface> surface;

    GpuSubmitOutcome operator()() noexcept {
        GpuSyncReadScope scope;
        return scope.withRead(surface, [](const GpuReadLease& lease) noexcept {
            (void) lease.nativeHandle();
            return GpuSubmitOutcome::NotSubmitted;
        });
    }
};

GpuSubmissionResult stubTypedSubmissionPass(std::shared_ptr<GpuFence> fence,
                                            std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    StubSubmissionAdapter adapter{surface};
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}
