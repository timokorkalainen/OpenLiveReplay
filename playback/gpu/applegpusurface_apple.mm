#include "playback/gpu/appleiosurface.h"

#ifdef __APPLE__

#include "playback/gpu/gpupipelineconfig.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

#include <cstring>
#include <memory>

namespace {

void zeroPixelBuffer(CVPixelBufferRef pb) {
    if (!pb) return;
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) return;

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

    CVPixelBufferUnlockBaseAddress(pb, 0);
}

class AppleGpuSurface final : public GpuSurface {
public:
    explicit AppleGpuSurface(CVPixelBufferRef pixelBuffer) : m_pixelBuffer(pixelBuffer) {}
    ~AppleGpuSurface() override {
        if (m_pixelBuffer) {
            CVPixelBufferRelease(m_pixelBuffer);
        }
    }

    GpuSurfaceDesc desc() const override {
        GpuSurfaceDesc d;
        d.format = FramePixelFormat::Nv12;
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

private:
    CVPixelBufferRef m_pixelBuffer = nullptr;
};

CFDictionaryRef makeIoSurfacePixelBufferAttributes() {
    CFDictionaryRef ioSurfaceProps =
        CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                           &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    if (!ioSurfaceProps) return nullptr;

    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {ioSurfaceProps};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                           &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(ioSurfaceProps);
    return attrs;
}

} // namespace

std::shared_ptr<GpuSurface> makeAppleNv12Surface(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    if (gpuConsumeInjectedAllocFailure()) return nullptr;

    CFDictionaryRef attrs = makeIoSurfacePixelBufferAttributes();
    if (!attrs) return nullptr;

    CVPixelBufferRef pb = nullptr;
    const CVReturn rc =
        CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &pb);
    CFRelease(attrs);
    if (rc != kCVReturnSuccess || !pb) return nullptr;

    zeroPixelBuffer(pb);
    auto surface = std::make_shared<AppleGpuSurface>(pb);
    return surface->isValid() ? surface : nullptr;
}

std::shared_ptr<GpuSurface> wrapAppleImageBuffer(void* cvImageBufferRef) {
    if (!cvImageBufferRef) return nullptr;

    auto pb = static_cast<CVPixelBufferRef>(cvImageBufferRef);
    if (!CVPixelBufferGetIOSurface(pb)) return nullptr;

    CVPixelBufferRetain(pb);
    auto surface = std::make_shared<AppleGpuSurface>(pb);
    return surface->isValid() ? surface : nullptr;
}

#endif // __APPLE__
