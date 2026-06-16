#ifndef NATIVEFRAMECOPY_H
#define NATIVEFRAMECOPY_H

#include <cstdint>

extern "C" {
struct AVFrame;
}

AVFrame* nativeCopyNv12ToYuv420p(const uint8_t* yPlane, int yStride,
                                 const uint8_t* uvPlane, int uvStride,
                                 int width, int height);

#endif // NATIVEFRAMECOPY_H
