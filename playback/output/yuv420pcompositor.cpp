#include "playback/output/yuv420pcompositor.h"

#include <QtGlobal>
#include <cmath>
#include <cstring>

namespace {
void copyPlane(const QByteArray& src, int srcStride, int srcW, int srcH, QByteArray& dst,
               int dstStride, int dstX, int dstY, int copyW, int copyH) {
    for (int y = 0; y < copyH && y < srcH; ++y) {
        std::memcpy(dst.data() + (dstY + y) * dstStride + dstX, src.constData() + y * srcStride,
                    size_t(qMin(copyW, srcW)));
    }
}
} // namespace

MediaVideoFrame Yuv420pCompositor::composeGrid(const QList<MediaVideoFrame>& frames, int width,
                                               int height) {
    MediaVideoFrame out = MediaVideoFrame::solidYuv420p(width, height, 16, 128, 128);
    out.feedIndex = -1;

    const int count = qMax(1, frames.size());
    const int columns = qMax(1, int(std::ceil(std::sqrt(double(count)))));
    const int rows = qMax(1, int(std::ceil(double(count) / double(columns))));
    const int tileW = width / columns;
    const int tileH = height / rows;

    for (int i = 0; i < frames.size(); ++i) {
        const MediaVideoFrame& frame = frames.at(i);
        if (!frame.isValid()) continue;
        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * tileW;
        const int dstY = row * tileH;

        copyPlane(frame.planeY, frame.strideY, frame.width, frame.height, out.planeY, out.strideY,
                  dstX, dstY, tileW, tileH);

        copyPlane(frame.planeU, frame.strideU, (frame.width + 1) / 2, (frame.height + 1) / 2,
                  out.planeU, out.strideU, dstX / 2, dstY / 2, (tileW + 1) / 2, (tileH + 1) / 2);
        copyPlane(frame.planeV, frame.strideV, (frame.width + 1) / 2, (frame.height + 1) / 2,
                  out.planeV, out.strideV, dstX / 2, dstY / 2, (tileW + 1) / 2, (tileH + 1) / 2);
    }
    return out;
}
