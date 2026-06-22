#include "playback/gpu/gpucompositor.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/formatcanon.h"

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>

#include <cstring>

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

std::shared_ptr<GpuSurface>
GpuCompositor::uploadFrameToNv12SurfaceForTest(const FrameHandle& frame,
                                               const std::shared_ptr<GpuRhiContext>& rhi) {
    if (!rhi || !rhi->isValid()) return nullptr;
    return uploadFrameToNv12Surface(frame);
}

#endif // __APPLE__
