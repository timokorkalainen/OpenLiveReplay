#ifndef OLR_APPLEIOSURFACE_H
#define OLR_APPLEIOSURFACE_H

#ifdef __APPLE__

#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <CoreVideo/CoreVideo.h>

#include <memory>

std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height);
std::shared_ptr<GpuSurface> makeAppleRgba8Surface(int width, int height);
std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef);
CVPixelBufferRef retainApplePixelBufferWrapper(const std::shared_ptr<GpuSurface>& surface);
CpuPlanes readAppleSurfaceToCpu(const std::shared_ptr<GpuSurface>& surface, FramePixelFormat target,
                                ColorMetadata color = ColorMetadata{});

#endif // __APPLE__

#endif // OLR_APPLEIOSURFACE_H
