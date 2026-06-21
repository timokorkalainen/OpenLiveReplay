#include "playback/output/yuv420pcompositor.h"

#include <QtGlobal>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
void scalePlaneNearest(const QByteArray& src, int srcStride, int srcW, int srcH, QByteArray& dst,
                       int dstStride, int dstX, int dstY, int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    for (int y = 0; y < dstH; ++y) {
        const int srcY = qMin(srcH - 1, (y * srcH) / dstH);
        char* dstLine = dst.data() + (dstY + y) * dstStride + dstX;
        const char* srcLine = src.constData() + srcY * srcStride;
        for (int x = 0; x < dstW; ++x) {
            const int srcX = qMin(srcW - 1, (x * srcW) / dstW);
            dstLine[x] = srcLine[srcX];
        }
    }
}
} // namespace

FrameHandle Yuv420pCompositor::composeGrid(const QList<FrameHandle>& frames, int width,
                                           int height) {
    FrameHandle outHandle = solidYuv420pHandle(width, height, 16, 128, 128);
    outHandle.metadata().key.feedIndex = -1;
    CpuPlanes out = outHandle.readToCpu(FramePixelFormat::Yuv420p);

    const int count = qMax(1, frames.size());
    const int columns = qMax(1, int(std::ceil(std::sqrt(double(count)))));
    const int rows = qMax(1, int(std::ceil(double(count) / double(columns))));

    for (int i = 0; i < frames.size(); ++i) {
        const MediaVideoFrameView frame(frames.at(i));
        if (!frame.isValid()) continue;
        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * width / columns;
        const int dstY = row * height / rows;
        const int dstRight = (col + 1) * width / columns;
        const int dstBottom = (row + 1) * height / rows;
        const int dstW = qMax(0, dstRight - dstX);
        const int dstH = qMax(0, dstBottom - dstY);

        scalePlaneNearest(frame.planeY, frame.strideY, frame.width, frame.height, out.plane[0],
                          out.stride[0], dstX, dstY, dstW, dstH);

        const int srcChromaW = (frame.width + 1) / 2;
        const int srcChromaH = (frame.height + 1) / 2;
        const int dstChromaX = dstX / 2;
        const int dstChromaY = dstY / 2;
        const int dstChromaRight = (dstRight + 1) / 2;
        const int dstChromaBottom = (dstBottom + 1) / 2;
        const int dstChromaW = qMax(0, dstChromaRight - dstChromaX);
        const int dstChromaH = qMax(0, dstChromaBottom - dstChromaY);
        scalePlaneNearest(frame.planeU, frame.strideU, srcChromaW, srcChromaH, out.plane[1],
                          out.stride[1], dstChromaX, dstChromaY, dstChromaW, dstChromaH);
        scalePlaneNearest(frame.planeV, frame.strideV, srcChromaW, srcChromaH, out.plane[2],
                          out.stride[2], dstChromaX, dstChromaY, dstChromaW, dstChromaH);
    }
    return makeCpuFrameHandle(std::move(out), outHandle.metadata());
}
