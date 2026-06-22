#ifndef OLR_VTKEEPSURFACEIMPORTER_H
#define OLR_VTKEEPSURFACEIMPORTER_H

#ifdef __APPLE__

#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <memory>

class GpuFence;

FrameHandle importVtImageBuffer(void* cvImageBufferRef, FrameMetadata meta,
                                std::shared_ptr<GpuRhiContext> rhi,
                                std::shared_ptr<GpuFence> renderFence = nullptr);
FrameHandle importVtSurface(const std::shared_ptr<GpuSurface>& surface, FrameMetadata meta,
                            std::shared_ptr<GpuRhiContext> rhi,
                            std::shared_ptr<GpuFence> renderFence = nullptr);

#endif // __APPLE__

#endif // OLR_VTKEEPSURFACEIMPORTER_H
