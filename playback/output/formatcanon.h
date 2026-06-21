#ifndef FORMATCANON_H
#define FORMATCANON_H

#include "playback/output/framehandle.h"
#include "playback/output/framepixelformat.h"

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

} // namespace formatcanon

#endif // FORMATCANON_H
