#include "playback/gpu/appleiosurface.h"

#ifdef __APPLE__

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/formatcanon.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>
#if __has_include(<IOSurface/IOSurfaceRef.h>)
#include <IOSurface/IOSurfaceRef.h>
#elif __has_include(<IOSurface/IOSurface.h>)
#include <IOSurface/IOSurface.h>
#endif

#include <atomic>
#include <cstring>
#include <memory>

namespace {

void zeroPixelBuffer(CVPixelBufferRef pb) {
    if (!pb) return;
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) return;

    if (CVPixelBufferIsPlanar(pb)) {
        const size_t planeCount = CVPixelBufferGetPlaneCount(pb);
        for (size_t plane = 0; plane < planeCount; ++plane) {
            auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddressOfPlane(pb, plane));
            const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
            const size_t height = CVPixelBufferGetHeightOfPlane(pb, plane);
            if (!base) continue;
            for (size_t row = 0; row < height; ++row) {
                std::memset(base + row * stride, 0, stride);
            }
        }
    } else {
        auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(pb));
        const size_t stride = CVPixelBufferGetBytesPerRow(pb);
        const size_t height = CVPixelBufferGetHeight(pb);
        if (base) {
            for (size_t row = 0; row < height; ++row) {
                std::memset(base + row * stride, 0, stride);
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(pb, 0);
}

qsizetype planeBytes(int stride, int rows) {
    return static_cast<qsizetype>(stride) * static_cast<qsizetype>(rows);
}

qsizetype byteOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

qint64 pixelBufferAllocationBytes(CVPixelBufferRef pb) {
    if (!pb) return 0;
    if (IOSurfaceRef ioSurface = CVPixelBufferGetIOSurface(pb)) {
        return static_cast<qint64>(IOSurfaceGetAllocSize(ioSurface));
    }
    qint64 bytes = 0;
    if (CVPixelBufferIsPlanar(pb)) {
        const size_t planeCount = CVPixelBufferGetPlaneCount(pb);
        for (size_t plane = 0; plane < planeCount; ++plane) {
            bytes += static_cast<qint64>(CVPixelBufferGetBytesPerRowOfPlane(pb, plane)) *
                     static_cast<qint64>(CVPixelBufferGetHeightOfPlane(pb, plane));
        }
    } else {
        bytes = static_cast<qint64>(CVPixelBufferGetBytesPerRow(pb)) *
                static_cast<qint64>(CVPixelBufferGetHeight(pb));
    }
    return bytes;
}

CpuPlanes lockDownloadNv12(CVPixelBufferRef pb) {
    CpuPlanes out;
    if (!pb || CVPixelBufferGetPlaneCount(pb) < 2) return out;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return out;

    const int w = static_cast<int>(CVPixelBufferGetWidth(pb));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pb));
    const int chromaW = (w + 1) / 2;
    const int chromaH = (h + 1) / 2;
    const auto* ySrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    const auto* uvSrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    if (!ySrc || !uvSrc || yStride < static_cast<size_t>(w) ||
        uvStride < static_cast<size_t>(chromaW * 2)) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return CpuPlanes{};
    }

    out.format = FramePixelFormat::Nv12;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = chromaW * 2;
    out.plane[0] = QByteArray(planeBytes(out.stride[0], h), '\0');
    out.plane[1] = QByteArray(planeBytes(out.stride[1], chromaH), '\0');

    for (int row = 0; row < h; ++row) {
        std::memcpy(out.plane[0].data() + byteOffset(row, out.stride[0]),
                    ySrc + static_cast<size_t>(row) * yStride, static_cast<size_t>(w));
    }
    for (int row = 0; row < chromaH; ++row) {
        std::memcpy(out.plane[1].data() + byteOffset(row, out.stride[1]),
                    uvSrc + static_cast<size_t>(row) * uvStride,
                    static_cast<size_t>(out.stride[1]));
    }

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return out;
}

CpuPlanes lockDownloadRgba8(CVPixelBufferRef pb) {
    CpuPlanes out;
    if (!pb) return out;
    const OSType pixelFormat = CVPixelBufferGetPixelFormatType(pb);
    if (pixelFormat != kCVPixelFormatType_32RGBA && pixelFormat != kCVPixelFormatType_32BGRA)
        return out;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return out;

    const int w = static_cast<int>(CVPixelBufferGetWidth(pb));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pb));
    const auto* src = static_cast<const uchar*>(CVPixelBufferGetBaseAddress(pb));
    const size_t srcStride = CVPixelBufferGetBytesPerRow(pb);
    if (!src || srcStride < static_cast<size_t>(w) * 4) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return CpuPlanes{};
    }

    out.format = FramePixelFormat::Rgba8;
    out.width = w;
    out.height = h;
    out.stride[0] = w * 4;
    out.plane[0] = QByteArray(planeBytes(out.stride[0], h), '\0');
    for (int row = 0; row < h; ++row) {
        const uchar* srcRow = src + static_cast<size_t>(row) * srcStride;
        char* dstRow = out.plane[0].data() + byteOffset(row, out.stride[0]);
        if (pixelFormat == kCVPixelFormatType_32RGBA) {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                const qsizetype offset = static_cast<qsizetype>(x) * 4;
                dstRow[offset] = char(srcRow[offset + 2]);
                dstRow[offset + 1] = char(srcRow[offset + 1]);
                dstRow[offset + 2] = char(srcRow[offset]);
                dstRow[offset + 3] = char(srcRow[offset + 3]);
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return out;
}

class AppleGpuSurface final : public GpuSurface {
public:
    AppleGpuSurface(CVPixelBufferRef pixelBuffer, FramePixelFormat format)
        : m_pixelBuffer(pixelBuffer), m_format(format),
          m_authorityEpoch(GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch()) {
        m_device = MTLCreateSystemDefaultDevice();
    }
    ~AppleGpuSurface() override {
        [m_device release];
        m_device = nil;
        if (m_pixelBuffer) {
            CVPixelBufferRelease(m_pixelBuffer);
        }
    }

    GpuSurfaceDesc desc() const override {
        GpuSurfaceDesc d;
        d.format = m_format;
        if (m_pixelBuffer) {
            d.width = static_cast<int>(CVPixelBufferGetWidth(m_pixelBuffer));
            d.height = static_cast<int>(CVPixelBufferGetHeight(m_pixelBuffer));
            d.allocationBytes = pixelBufferAllocationBytes(m_pixelBuffer);
        }
        return d;
    }

    bool isValid() const override { return m_pixelBuffer != nullptr && nativeHandle() != nullptr; }
    GpuSurfaceCompatibility compatibility() const override {
        return {reinterpret_cast<uintptr_t>((__bridge void*)m_device), m_authorityEpoch};
    }

    void retainUntilFenceRetired(uint64_t fenceValue) override {
        uint64_t previous = m_pendingFence.load(std::memory_order_acquire);
        while (fenceValue > previous && !m_pendingFence.compare_exchange_weak(
                                            previous, fenceValue, std::memory_order_acq_rel)) {
        }
    }

    uint64_t pendingFenceValue() const override {
        return m_pendingFence.load(std::memory_order_acquire);
    }

protected:
    // Lease-gated, mirroring the base (gpusurface.h). Kept protected on the
    // derived type too so an AppleGpuSurface* cannot re-widen handle access.
    void* nativeHandle() const override {
        return m_pixelBuffer ? CVPixelBufferGetIOSurface(m_pixelBuffer) : nullptr;
    }
    GpuOwnedNativeHandle retainNativeHandle() const override {
        IOSurfaceRef surface = m_pixelBuffer ? CVPixelBufferGetIOSurface(m_pixelBuffer) : nullptr;
        if (!surface) return {};
        CFRetain(surface);
        return GpuOwnedNativeHandle::adopt(
            surface, [](void* value) { CFRelease(static_cast<CFTypeRef>(value)); });
    }

private:
    CVPixelBufferRef m_pixelBuffer = nullptr;
    id<MTLDevice> m_device = nil;
    FramePixelFormat m_format = FramePixelFormat::Nv12;
    uint64_t m_authorityEpoch = 0;
    std::atomic<uint64_t> m_pendingFence{0};
};

CFDictionaryRef makeIoSurfacePixelBufferAttributes(bool metalCompatible) {
    CFDictionaryRef ioSurfaceProps =
        CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    if (!ioSurfaceProps) return nullptr;

    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey,
                          kCVPixelBufferMetalCompatibilityKey};
    const void* values[] = {ioSurfaceProps, kCFBooleanTrue};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, metalCompatible ? 2 : 1,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(ioSurfaceProps);
    return attrs;
}

CVPixelBufferRef createIoSurfacePixelBuffer(int width, int height, OSType pixelFormat) {
    for (const bool metalCompatible : {true, false}) {
        CFDictionaryRef attrs = makeIoSurfacePixelBufferAttributes(metalCompatible);
        if (!attrs) return nullptr;

        CVPixelBufferRef pb = nullptr;
        const CVReturn rc =
            CVPixelBufferCreate(kCFAllocatorDefault, width, height, pixelFormat, attrs, &pb);
        CFRelease(attrs);
        if (rc == kCVReturnSuccess && pb && CVPixelBufferGetIOSurface(pb)) return pb;
        if (pb) CVPixelBufferRelease(pb);
    }
    return nullptr;
}

} // namespace

std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    if (gpuConsumeInjectedAllocFailure()) return nullptr;

    CVPixelBufferRef pb =
        createIoSurfacePixelBuffer(width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    if (!pb) return nullptr;

    zeroPixelBuffer(pb);
    auto surface = std::make_shared<AppleGpuSurface>(pb, FramePixelFormat::Nv12);
    return surface->isValid() ? surface : nullptr;
}

std::shared_ptr<GpuSurface> makeAppleRgba8Surface(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    if (gpuConsumeInjectedAllocFailure()) return nullptr;

    CVPixelBufferRef pb = createIoSurfacePixelBuffer(width, height, kCVPixelFormatType_32BGRA);
    if (!pb) return nullptr;

    zeroPixelBuffer(pb);
    auto surface = std::make_shared<AppleGpuSurface>(pb, FramePixelFormat::Rgba8);
    return surface->isValid() ? surface : nullptr;
}

std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef) {
    if (!cvImageBufferRef) return nullptr;

    auto pb = static_cast<CVPixelBufferRef>(cvImageBufferRef);
    if (!CVPixelBufferGetIOSurface(pb)) return nullptr;

    CVPixelBufferRetain(pb);
    auto surface = std::make_shared<AppleGpuSurface>(pb, FramePixelFormat::Nv12);
    return surface->isValid() ? surface : nullptr;
}

CVPixelBufferRef retainApplePixelBufferWrapper(const std::shared_ptr<GpuSurface>& surface) {
    if (!surface || !surface->isValid()) return nullptr;
    GpuSyncReadScope scope;
    return scope.withRead(surface, [](const GpuReadLease& lease) {
        IOSurfaceRef ioSurface = static_cast<IOSurfaceRef>(lease.nativeHandle());
        if (!ioSurface) return static_cast<CVPixelBufferRef>(nullptr);

        CVPixelBufferRef pixelBuffer = nullptr;
        const CVReturn result =
            CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pixelBuffer);
        return result == kCVReturnSuccess ? pixelBuffer : nullptr;
    });
}

CpuPlanes readAppleSurfaceToCpu(const std::shared_ptr<GpuSurface>& surface, FramePixelFormat target,
                                ColorMetadata color) {
    if (!surface || !surface->isValid()) return CpuPlanes{};
    const GpuSurfaceDesc desc = surface->desc();
    CVPixelBufferRef pb = retainApplePixelBufferWrapper(surface);
    if (!pb) return CpuPlanes{};

    CpuPlanes result;
    if (desc.format == FramePixelFormat::Nv12) {
        CpuPlanes nv12 = lockDownloadNv12(pb);
        if (target == FramePixelFormat::Nv12) {
            result = std::move(nv12);
        } else if (target == FramePixelFormat::Yuv420p) {
            result = formatcanon::nv12ToYuv420p(nv12);
        }
    } else if (desc.format == FramePixelFormat::Rgba8) {
        CpuPlanes rgba = lockDownloadRgba8(pb);
        if (target == FramePixelFormat::Rgba8) {
            result = std::move(rgba);
        } else if (target == FramePixelFormat::Yuv420p) {
            result = formatcanon::exportRgba8ToYuv420p(rgba, color);
        } else if (target == FramePixelFormat::Nv12) {
            result = formatcanon::exportRgba8ToNv12(rgba, color);
        }
    }

    CVPixelBufferRelease(pb);
    return result;
}

#endif // __APPLE__
