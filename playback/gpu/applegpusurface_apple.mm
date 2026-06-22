#include "playback/gpu/appleiosurface.h"

#ifdef __APPLE__

#include "playback/gpu/gpupipelineconfig.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

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

class AppleGpuSurface final : public GpuSurface {
public:
    AppleGpuSurface(CVPixelBufferRef pixelBuffer, FramePixelFormat format)
        : m_pixelBuffer(pixelBuffer), m_format(format) {}
    ~AppleGpuSurface() override {
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
        }
        return d;
    }

    bool isValid() const override { return m_pixelBuffer != nullptr && nativeHandle() != nullptr; }

    void* nativeHandle() const override {
        return m_pixelBuffer ? CVPixelBufferGetIOSurface(m_pixelBuffer) : nullptr;
    }

    void retainUntilFenceRetired(uint64_t fenceValue) override {
        uint64_t previous = m_pendingFence.load(std::memory_order_acquire);
        while (fenceValue > previous &&
               !m_pendingFence.compare_exchange_weak(previous, fenceValue,
                                                      std::memory_order_acq_rel)) {
        }
    }

    uint64_t pendingFenceValue() const override {
        return m_pendingFence.load(std::memory_order_acquire);
    }

private:
    CVPixelBufferRef m_pixelBuffer = nullptr;
    FramePixelFormat m_format = FramePixelFormat::Nv12;
    std::atomic<uint64_t> m_pendingFence{0};
};

CFDictionaryRef makeIoSurfacePixelBufferAttributes(bool metalCompatible) {
    CFDictionaryRef ioSurfaceProps =
        CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                           &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    if (!ioSurfaceProps) return nullptr;

    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey,
                          kCVPixelBufferMetalCompatibilityKey};
    const void* values[] = {ioSurfaceProps, kCFBooleanTrue};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, metalCompatible ? 2 : 1,
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

    CVPixelBufferRef pb = createIoSurfacePixelBuffer(
        width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
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

#endif // __APPLE__
