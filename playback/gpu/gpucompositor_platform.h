#ifndef OLR_GPUCOMPOSITOR_PLATFORM_H
#define OLR_GPUCOMPOSITOR_PLATFORM_H

#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <rhi/qrhi.h>

#include <memory>

namespace gpucompositor {

class ImportedRgbaRenderTarget {
public:
    virtual ~ImportedRgbaRenderTarget() = default;
    virtual QRhiTexture::NativeTexture nativeTexture() const = 0;
};

class ImportedNv12Source {
public:
    virtual ~ImportedNv12Source() = default;
    virtual QRhiTexture::NativeTexture lumaNativeTexture() const = 0;
    virtual QRhiTexture::NativeTexture chromaNativeTexture() const = 0;
};

std::shared_ptr<GpuSurface> makeInputNv12Surface(const FrameHandle& frame);
std::shared_ptr<GpuSurface> makeOutputRgba8Surface(int width, int height);
std::unique_ptr<ImportedNv12Source> importNv12Source(QRhi* rhi,
                                                     const std::shared_ptr<GpuSurface>& surface);
std::unique_ptr<ImportedRgbaRenderTarget>
importRgbaRenderTarget(QRhi* rhi, const std::shared_ptr<GpuSurface>& surface);

} // namespace gpucompositor

#endif // OLR_GPUCOMPOSITOR_PLATFORM_H
