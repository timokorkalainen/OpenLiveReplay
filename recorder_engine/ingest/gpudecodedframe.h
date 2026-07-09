#ifndef OLR_GPUDECODEDFRAME_H
#define OLR_GPUDECODEDFRAME_H

#include "h26xaccessunit.h"
#include "playback/output/framehandle.h"

FrameMetadata gpuDecodedFrameMetadata(const CompressedAccessUnit& unit, int width, int height,
                                      qint64 ptsMs);
FrameHandle makeGpuDecodedFrameHandle(void* nativeDecodedImage, const CompressedAccessUnit& unit,
                                      int width, int height, qint64 ptsMs);

#endif // OLR_GPUDECODEDFRAME_H
