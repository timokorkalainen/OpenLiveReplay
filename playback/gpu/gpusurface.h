#ifndef OLR_GPUSURFACE_H
#define OLR_GPUSURFACE_H

#include "playback/output/framepixelformat.h"

struct GpuSurfaceDesc {
    FramePixelFormat format = FramePixelFormat::Nv12;
    int width = 0;
    int height = 0;
};

// GPU-resident pixel surface behind the opaque GpuSurface forward declaration
// in framehandle.h. This shared header intentionally exposes no platform SDK
// types; platform import code downcasts nativeHandle() inside .mm/.cpp files.
class GpuSurface {
public:
    virtual ~GpuSurface();

    virtual GpuSurfaceDesc desc() const = 0;
    virtual bool isValid() const = 0;

    // IOSurfaceRef on Apple, ID3D11Texture2D* on Windows.
    virtual void* nativeHandle() const = 0;
};

#endif // OLR_GPUSURFACE_H
