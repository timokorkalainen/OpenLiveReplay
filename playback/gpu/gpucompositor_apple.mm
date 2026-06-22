#include "playback/gpu/gpucompositor.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpucompositor_platform.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/formatcanon.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <Metal/Metal.h>
#include <rhi/qrhi_platform.h>

#include <cstring>
#include <memory>

namespace {

qsizetype byteOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

bool hasRows(const QByteArray& plane, int stride, int rows, int bytesPerRow) {
    if (rows <= 0) return true;
    if (stride < bytesPerRow || bytesPerRow < 0) return false;
    const qsizetype lastByte =
        byteOffset(rows - 1, stride) + static_cast<qsizetype>(bytesPerRow);
    return plane.size() >= lastByte;
}

std::shared_ptr<GpuSurface> aliasGpuNv12Surface(const FrameHandle& frame) {
    if (!frame.isGpuBacked()) return nullptr;
    auto* data = dynamic_cast<const GpuFrameData*>(frame.data());
    if (!data || data->nativeFormat() != FramePixelFormat::Nv12) return nullptr;
    if (!data->waitForPendingFence(-1)) return nullptr;

    auto surface = data->surfacePtr();
    if (!surface || !surface->isValid()) return nullptr;
    const GpuSurfaceDesc desc = surface->desc();
    if (desc.format != FramePixelFormat::Nv12 || desc.width <= 0 || desc.height <= 0) {
        return nullptr;
    }
    return surface;
}

CpuPlanes readFrameAsNv12(const FrameHandle& frame) {
    CpuPlanes nv12 = frame.readToCpu(FramePixelFormat::Nv12);
    if (nv12.format == FramePixelFormat::Nv12 && nv12.isValid()) return nv12;

    const CpuPlanes yuv = frame.readToCpu(FramePixelFormat::Yuv420p);
    return formatcanon::yuv420pToNv12(yuv);
}

bool copyNv12ToPixelBuffer(const CpuPlanes& nv12, CVPixelBufferRef pb) {
    if (!nv12.isValid() || nv12.format != FramePixelFormat::Nv12 || !pb) return false;
    if (CVPixelBufferGetPlaneCount(pb) < 2) return false;

    const int width = nv12.width;
    const int height = nv12.height;
    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;
    const int uvBytesPerRow = chromaW * 2;
    if (!hasRows(nv12.plane[0], nv12.stride[0], height, width) ||
        !hasRows(nv12.plane[1], nv12.stride[1], chromaH, uvBytesPerRow)) {
        return false;
    }

    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) return false;

    bool ok = true;
    auto* yDst = static_cast<uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    auto* uvDst = static_cast<uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    const size_t yRows = CVPixelBufferGetHeightOfPlane(pb, 0);
    const size_t uvRows = CVPixelBufferGetHeightOfPlane(pb, 1);

    if (!yDst || !uvDst || yStride < static_cast<size_t>(width) ||
        uvStride < static_cast<size_t>(uvBytesPerRow) || yRows < static_cast<size_t>(height) ||
        uvRows < static_cast<size_t>(chromaH)) {
        ok = false;
    }

    if (ok) {
        for (int row = 0; row < height; ++row) {
            std::memcpy(yDst + static_cast<size_t>(row) * yStride,
                        nv12.plane[0].constData() + byteOffset(row, nv12.stride[0]),
                        static_cast<size_t>(width));
        }
        for (int row = 0; row < chromaH; ++row) {
            std::memcpy(uvDst + static_cast<size_t>(row) * uvStride,
                        nv12.plane[1].constData() + byteOffset(row, nv12.stride[1]),
                        static_cast<size_t>(uvBytesPerRow));
        }
    }

    CVPixelBufferUnlockBaseAddress(pb, 0);
    return ok;
}

std::shared_ptr<GpuSurface> uploadFrameToNv12Surface(const FrameHandle& frame) {
    if (auto aliased = aliasGpuNv12Surface(frame)) return aliased;
    if (frame.isGpuBacked()) return nullptr;

    const CpuPlanes nv12 = readFrameAsNv12(frame);
    if (!nv12.isValid() || nv12.format != FramePixelFormat::Nv12) return nullptr;

    auto surface = makeAppleNv12Surface(nv12.width, nv12.height);
    if (!surface || !surface->isValid()) return nullptr;

    auto ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    if (!ioSurface) return nullptr;

    CVPixelBufferRef pb = nullptr;
    const CVReturn created =
        CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pb);
    if (created != kCVReturnSuccess || !pb) return nullptr;

    const bool copied = copyNv12ToPixelBuffer(nv12, pb);
    CVPixelBufferRelease(pb);
    return copied ? surface : nullptr;
}

} // namespace

namespace gpucompositor {

class AppleImportedRgbaRenderTarget final : public ImportedRgbaRenderTarget {
public:
    AppleImportedRgbaRenderTarget(CVPixelBufferRef pixelBuffer, CVMetalTextureCacheRef cache,
                                  CVMetalTextureRef texture)
        : m_pixelBuffer(pixelBuffer), m_cache(cache), m_texture(texture) {}

    ~AppleImportedRgbaRenderTarget() override {
        if (m_texture) CFRelease(m_texture);
        if (m_cache) CFRelease(m_cache);
        if (m_pixelBuffer) CVPixelBufferRelease(m_pixelBuffer);
    }

    QRhiTexture::NativeTexture nativeTexture() const override {
        id<MTLTexture> texture = m_texture ? CVMetalTextureGetTexture(m_texture) : nil;
        return QRhiTexture::NativeTexture{
            quint64(reinterpret_cast<uintptr_t>((__bridge void*) texture)), 0};
    }

private:
    CVPixelBufferRef m_pixelBuffer = nullptr;
    CVMetalTextureCacheRef m_cache = nullptr;
    CVMetalTextureRef m_texture = nullptr;
};

class AppleImportedNv12Source final : public ImportedNv12Source {
public:
    AppleImportedNv12Source(CVPixelBufferRef pixelBuffer, CVMetalTextureCacheRef cache,
                            CVMetalTextureRef luma, CVMetalTextureRef chroma)
        : m_pixelBuffer(pixelBuffer), m_cache(cache), m_luma(luma), m_chroma(chroma) {}

    ~AppleImportedNv12Source() override {
        if (m_chroma) CFRelease(m_chroma);
        if (m_luma) CFRelease(m_luma);
        if (m_cache) CFRelease(m_cache);
        if (m_pixelBuffer) CVPixelBufferRelease(m_pixelBuffer);
    }

    QRhiTexture::NativeTexture lumaNativeTexture() const override {
        id<MTLTexture> texture = m_luma ? CVMetalTextureGetTexture(m_luma) : nil;
        return QRhiTexture::NativeTexture{
            quint64(reinterpret_cast<uintptr_t>((__bridge void*) texture)), 0};
    }

    QRhiTexture::NativeTexture chromaNativeTexture() const override {
        id<MTLTexture> texture = m_chroma ? CVMetalTextureGetTexture(m_chroma) : nil;
        return QRhiTexture::NativeTexture{
            quint64(reinterpret_cast<uintptr_t>((__bridge void*) texture)), 0};
    }

private:
    CVPixelBufferRef m_pixelBuffer = nullptr;
    CVMetalTextureCacheRef m_cache = nullptr;
    CVMetalTextureRef m_luma = nullptr;
    CVMetalTextureRef m_chroma = nullptr;
};

CVPixelBufferRef makePixelBufferWrapper(const std::shared_ptr<GpuSurface>& surface) {
    if (!surface || !surface->isValid()) return nullptr;
    auto ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    if (!ioSurface) return nullptr;

    CVPixelBufferRef pb = nullptr;
    const CVReturn created =
        CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pb);
    return created == kCVReturnSuccess ? pb : nullptr;
}

CVMetalTextureCacheRef makeTextureCache(QRhi* rhi) {
    if (!rhi) return nullptr;
    const auto* nativeHandles = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
    if (!nativeHandles || !nativeHandles->dev) return nullptr;
    id<MTLDevice> device = (__bridge id<MTLDevice>) nativeHandles->dev;

    CVMetalTextureCacheRef cache = nullptr;
    const CVReturn rc =
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &cache);
    return rc == kCVReturnSuccess ? cache : nullptr;
}

CFDictionaryRef makeMetalTextureAttributes(MTLTextureUsage usage) {
    uint64_t usageValue = static_cast<uint64_t>(usage);
    CFNumberRef usageNumber =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &usageValue);
    if (!usageNumber) return nullptr;

    const void* keys[] = {kCVMetalTextureUsage};
    const void* values[] = {usageNumber};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                           &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(usageNumber);
    return attrs;
}

std::shared_ptr<GpuSurface> makeInputNv12Surface(const FrameHandle& frame) {
    return uploadFrameToNv12Surface(frame);
}

std::shared_ptr<GpuSurface> makeOutputRgba8Surface(int width, int height) {
    return makeAppleRgba8Surface(width, height);
}

std::unique_ptr<ImportedNv12Source> importNv12Source(QRhi* rhi,
                                                     const std::shared_ptr<GpuSurface>& surface) {
    if (!rhi || !surface || !surface->isValid()) return nullptr;
    const GpuSurfaceDesc desc = surface->desc();
    if (desc.format != FramePixelFormat::Nv12 || desc.width <= 0 || desc.height <= 0) {
        return nullptr;
    }

    CVPixelBufferRef pb = makePixelBufferWrapper(surface);
    if (!pb) return nullptr;

    CVMetalTextureCacheRef cache = makeTextureCache(rhi);
    if (!cache) {
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    CFDictionaryRef readAttrs = makeMetalTextureAttributes(MTLTextureUsageShaderRead);
    if (!readAttrs) {
        CFRelease(cache);
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    CVMetalTextureRef luma = nullptr;
    CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, pb, readAttrs, MTLPixelFormatR8Unorm, desc.width, desc.height,
        0, &luma);
    if (rc != kCVReturnSuccess || !luma) {
        CFRelease(readAttrs);
        CFRelease(cache);
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    CVMetalTextureRef chroma = nullptr;
    rc = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, readAttrs,
                                                   MTLPixelFormatRG8Unorm, (desc.width + 1) / 2,
                                                   (desc.height + 1) / 2, 1, &chroma);
    CFRelease(readAttrs);
    if (rc != kCVReturnSuccess || !chroma) {
        CFRelease(luma);
        CFRelease(cache);
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    auto imported = std::make_unique<AppleImportedNv12Source>(pb, cache, luma, chroma);
    return imported->lumaNativeTexture().object != 0 && imported->chromaNativeTexture().object != 0
               ? std::move(imported)
               : nullptr;
}

std::unique_ptr<ImportedRgbaRenderTarget>
importRgbaRenderTarget(QRhi* rhi, const std::shared_ptr<GpuSurface>& surface) {
    if (!rhi || !surface || !surface->isValid()) return nullptr;
    const GpuSurfaceDesc desc = surface->desc();
    if (desc.format != FramePixelFormat::Rgba8 || desc.width <= 0 || desc.height <= 0) {
        return nullptr;
    }

    CVPixelBufferRef pb = makePixelBufferWrapper(surface);
    if (!pb) return nullptr;

    CVMetalTextureCacheRef cache = makeTextureCache(rhi);
    if (!cache) {
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    CFDictionaryRef renderAttrs =
        makeMetalTextureAttributes(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    if (!renderAttrs) {
        CFRelease(cache);
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    CVMetalTextureRef texture = nullptr;
    CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb,
                                                            renderAttrs,
                                                            MTLPixelFormatBGRA8Unorm, desc.width,
                                                            desc.height, 0, &texture);
    CFRelease(renderAttrs);
    if (rc != kCVReturnSuccess || !texture) {
        CFRelease(cache);
        CVPixelBufferRelease(pb);
        return nullptr;
    }

    auto imported = std::make_unique<AppleImportedRgbaRenderTarget>(pb, cache, texture);
    return imported->nativeTexture().object != 0 ? std::move(imported) : nullptr;
}

} // namespace gpucompositor

std::shared_ptr<GpuSurface>
GpuCompositor::uploadFrameToNv12SurfaceForTest(const FrameHandle& frame,
                                               const std::shared_ptr<GpuRhiContext>& rhi) {
    if (!rhi || !rhi->isValid()) return nullptr;
    return uploadFrameToNv12Surface(frame);
}

#endif // __APPLE__
