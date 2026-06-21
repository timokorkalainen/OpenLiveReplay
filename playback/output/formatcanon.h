#ifndef FORMATCANON_H
#define FORMATCANON_H

#include "playback/output/framepixelformat.h"

namespace formatcanon {

struct PlaneShape {
    int width = 0;
    int height = 0;
};

PlaneShape planeShape(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);
int bytesPerSample(FramePixelFormat format, int planeIndex);
int packedStride(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);

} // namespace formatcanon

#endif // FORMATCANON_H
