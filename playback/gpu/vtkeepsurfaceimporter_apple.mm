#include "playback/gpu/vtkeepsurfaceimporter.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"

#include <array>
#include <utility>

FrameHandle importVtImageBuffer(void* cvImageBufferRef, FrameMetadata meta,
                                std::shared_ptr<GpuRhiContext> rhi,
                                std::shared_ptr<GpuFence> renderFence) {
    if (!rhi || !rhi->isGpuBacked()) return FrameHandle();
    const GpuSurfaceCompatibility compatibility = rhi->surfaceCompatibility();
    if (compatibility.deviceDomainId == 0 || compatibility.authorityEpoch == 0)
        return FrameHandle();
    std::shared_ptr<GpuSurface> surface = wrapAppleImageBuffer(cvImageBufferRef, compatibility);
    if (!surface) return FrameHandle();
    return importVtSurface(surface, std::move(meta), std::move(rhi), std::move(renderFence));
}

FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface,
                            FrameMetadata meta,
                            std::shared_ptr<GpuRhiContext> rhi,
                            std::shared_ptr<GpuFence> renderFence) {
    if (!surface || !surface->isValid() || !rhi || !rhi->isGpuBacked()) return FrameHandle();
    const GpuSurfaceCompatibility expected = rhi->surfaceCompatibility();
    const GpuSurfaceCompatibility actual = surface->compatibility();
    if (expected.deviceDomainId == 0 || expected.authorityEpoch == 0 ||
        actual.deviceDomainId != expected.deviceDomainId ||
        actual.authorityEpoch != expected.authorityEpoch)
        return FrameHandle();

    const GpuSurfaceDesc desc = surface->desc();
    auto charge = GpuBudget::instance().tryCharge(gpuSurfaceBytes(*surface));
    if (!charge) {
        GpuBudget::instance().noteOomDegrade();
        return FrameHandle();
    }
    meta.key.format = FramePixelFormat::Nv12;
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    uint64_t renderFenceValue = 0;
    std::shared_ptr<GpuFence> exactRenderFence;
    if (renderFence) {
        GpuRetireRegistry registry;
        GpuOpScope operation(renderFence, registry);
        auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
        const auto result = operation.submitRetained(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        if (!result.succeeded()) return FrameHandle{};
        exactRenderFence = result.producerFence;
        renderFenceValue = result.fenceValue;
    }
    return makeGpuFrameHandle(surface, std::move(rhi), std::move(meta), std::move(exactRenderFence),
                              renderFenceValue, std::move(*charge));
}

#endif // __APPLE__
