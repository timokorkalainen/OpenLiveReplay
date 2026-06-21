#include "playback/gpu/vtkeepsurfaceimporter.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"

#include <utility>

FrameHandle importVtImageBuffer(void* cvImageBufferRef, FrameMetadata meta,
                                std::shared_ptr<GpuRhiContext> rhi) {
    std::shared_ptr<GpuSurface> surface = wrapAppleImageBuffer(cvImageBufferRef);
    if (!surface || !rhi) return FrameHandle();
    return importVtSurface(surface, std::move(meta), std::move(rhi));
}

FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface,
                            FrameMetadata meta,
                            std::shared_ptr<GpuRhiContext> rhi) {
    if (!surface || !surface->isValid() || !rhi) return FrameHandle();

    const GpuSurfaceDesc desc = surface->desc();
    meta.key.format = FramePixelFormat::Nv12;
    if (meta.key.width <= 0) meta.key.width = desc.width;
    if (meta.key.height <= 0) meta.key.height = desc.height;
    return makeGpuFrameHandle(surface, std::move(rhi), std::move(meta));
}

#endif // __APPLE__
