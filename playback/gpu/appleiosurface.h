#ifndef OLR_APPLEIOSURFACE_H
#define OLR_APPLEIOSURFACE_H

#ifdef __APPLE__

#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/framehandle.h"

#include <CoreVideo/CoreVideo.h>

#include <memory>

std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height,
                                                 GpuSurfaceCompatibility compatibility = {});
std::shared_ptr<GpuSurface> makeAppleRgba8Surface(int width, int height,
                                                  GpuSurfaceCompatibility compatibility = {});
std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef,
                                                 GpuSurfaceCompatibility compatibility = {});
CVPixelBufferRef retainApplePixelBufferWrapper(const std::shared_ptr<GpuSurface>& surface);
CVPixelBufferRef retainApplePixelBufferWrapper(const GpuScopedNativeSurface& surface);
CpuPlanes readAppleSurfaceToCpu(const std::shared_ptr<GpuSurface>& surface, FramePixelFormat target,
                                ColorMetadata color = ColorMetadata{});

#endif // __APPLE__

#endif // OLR_APPLEIOSURFACE_H
