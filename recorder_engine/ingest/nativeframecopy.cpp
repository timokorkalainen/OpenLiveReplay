#include "nativeframecopy.h"

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

void copyRows(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride,
              int bytesPerRow, int rows) {
    for (int y = 0; y < rows; ++y) {
        memcpy(dst + y * dstStride, src + y * srcStride, size_t(bytesPerRow));
    }
}

} // namespace

AVFrame* nativeCopyNv12ToYuv420p(const uint8_t* yPlane, int yStride,
                                 const uint8_t* uvPlane, int uvStride,
                                 int width, int height) {
    if (!yPlane || !uvPlane || width <= 0 || height <= 0 ||
        (width % 2) != 0 || (height % 2) != 0 ||
        yStride <= 0 || uvStride <= 0 ||
        yStride < width || uvStride < width) {
        return nullptr;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    copyRows(yPlane, yStride, frame->data[0], frame->linesize[0], width, height);
    for (int row = 0; row < height / 2; ++row) {
        const uint8_t* src = uvPlane + row * uvStride;
        uint8_t* u = frame->data[1] + row * frame->linesize[1];
        uint8_t* v = frame->data[2] + row * frame->linesize[2];
        for (int col = 0; col < width / 2; ++col) {
            u[col] = src[col * 2];
            v[col] = src[col * 2 + 1];
        }
    }
    return frame;
}
