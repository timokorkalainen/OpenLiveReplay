#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/vtkeepsurfaceimporter.h"

#include <array>
#include <memory>

struct MetalSubmissionAdapter {
    std::shared_ptr<GpuSurface> surface;

    GpuSubmitOutcome operator()() noexcept {
        GpuSyncReadScope scope;
        return scope.withRead(surface, [](const GpuReadLease& lease) noexcept {
            IOSurfaceRef ioSurface = static_cast<IOSurfaceRef>(lease.nativeHandle());
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            return ioSurface && device ? GpuSubmitOutcome::Submitted
                                       : GpuSubmitOutcome::NotSubmitted;
        });
    }
};

GpuSubmissionResult metalTypedSubmissionPass(std::shared_ptr<GpuFence> fence,
                                             std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    MetalSubmissionAdapter adapter{surface};
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}

FrameHandle metalMigratedCallSitePass(std::shared_ptr<GpuSurface> surface, FrameMetadata metadata,
                                      std::shared_ptr<GpuRhiContext> rhi) {
    return importVtSurface(std::move(surface), metadata, std::move(rhi));
}
