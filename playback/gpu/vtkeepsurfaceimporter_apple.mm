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
    std::shared_ptr<GpuSurface> surface = wrapAppleImageBuffer(cvImageBufferRef);
    if (!surface || !rhi) return FrameHandle();
    return importVtSurface(surface, std::move(meta), std::move(rhi), std::move(renderFence));
}

FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface,
                            FrameMetadata meta,
                            std::shared_ptr<GpuRhiContext> rhi,
                            std::shared_ptr<GpuFence> renderFence) {
    if (!surface || !surface->isValid() || !rhi) return FrameHandle();

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
    if (renderFence) {
        GpuRetireRegistry registry;
        GpuOpScope operation(renderFence, registry);
        auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
        const auto result = operation.submit(
            adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
        if (!result.succeeded()) return FrameHandle{};
        renderFenceValue = result.fenceValue;
    }
    return makeGpuFrameHandle(surface, std::move(rhi), std::move(meta), std::move(renderFence),
                              renderFenceValue, std::move(*charge));
}

#endif // __APPLE__
