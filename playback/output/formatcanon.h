#ifndef FORMATCANON_H
#define FORMATCANON_H

#include "playback/output/framehandle.h"
#include "playback/output/framepixelformat.h"

#include <QList>

namespace formatcanon {

struct PlaneShape {
    int width = 0;
    int height = 0;
};

PlaneShape planeShape(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);
int bytesPerSample(FramePixelFormat format, int planeIndex);
int packedStride(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);
CpuPlanes nv12ToYuv420p(const CpuPlanes& src);
CpuPlanes yuv420pToNv12(const CpuPlanes& src);

struct FullResChroma {
    int width = 0;
    int height = 0;
    QByteArray u;
    QByteArray v;
};

FullResChroma upsampleChromaNearest(const CpuPlanes& yuv420p);

struct Rgb8 {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
};

Rgb8 yuvToRgb8(uchar y, uchar u, uchar v, ColorMatrix matrix, ColorRange range);
CpuPlanes referenceComposeGridRgba8(const QList<FrameHandle>& frames, int width, int height,
                                    ColorMetadata color);

} // namespace formatcanon

#endif // FORMATCANON_H
