#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/wingpuimportedge.h"

#include <array>
#include <memory>

struct D3D11SubmissionAdapter {
    std::shared_ptr<D3D11GpuSurface> surface;

    GpuSubmitOutcome operator()() noexcept {
        GpuSubmitOutcome outcome = GpuSubmitOutcome::NotSubmitted;
        GpuSyncReadScope scope;
        scope.withRead(surface, [&](const GpuReadLease& lease) noexcept {
            auto* texture = static_cast<ID3D11Texture2D*>(lease.nativeHandle());
            outcome = texture ? GpuSubmitOutcome::Submitted : GpuSubmitOutcome::NotSubmitted;
        });
        return outcome;
    }
};

GpuSubmissionResult d3d11TypedSubmissionPass(std::shared_ptr<GpuFence> fence,
                                             std::shared_ptr<D3D11GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    D3D11SubmissionAdapter adapter{surface};
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}

FrameHandle d3d11MigratedCallSitePass(std::shared_ptr<D3D11GpuSurface> surface,
                                      FrameMetadata metadata, std::shared_ptr<GpuFence> fence) {
    return WinGpuImportEdge::makeGpuFrameHandleForTest(std::move(surface), metadata,
                                                       std::move(fence));
}
