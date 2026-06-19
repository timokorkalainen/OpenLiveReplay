#include "recorder_engine/benchmark/syntheticframes.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

AVFrame* makeSyntheticFrame(int width, int height, int seq) {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = width;
    f->height = height;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return nullptr;
    }

    // --- Y plane: diagonal gradient + per-seq phase shift ---
    for (int y = 0; y < height; ++y) {
        uint8_t* row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < width; ++x)
            row[x] = static_cast<uint8_t>((x + y + seq * 4) & 0xff);
    }

    // --- Moving 32×32 contrast block (gives inter-frame motion) ---
    const int bx = width > 32 ? (seq * 8) % (width - 32) : 0;
    const int by = height > 32 ? (seq * 4) % (height - 32) : 0;
    const int bw = (bx + 32 <= width) ? 32 : width - bx;
    const int bh = (by + 32 <= height) ? 32 : height - by;
    for (int y = by; y < by + bh; ++y) {
        uint8_t* row = f->data[0] + y * f->linesize[0];
        for (int x = bx; x < bx + bw; ++x)
            row[x] = static_cast<uint8_t>(~row[x]); // invert for high contrast
    }

    // --- U plane: mild horizontal gradient ---
    const int hw = (width + 1) / 2;
    const int hh = (height + 1) / 2;
    for (int y = 0; y < hh; ++y) {
        uint8_t* row = f->data[1] + y * f->linesize[1];
        for (int x = 0; x < hw; ++x)
            row[x] = static_cast<uint8_t>((x * 2) & 0xff);
    }

    // --- V plane: mild vertical gradient ---
    for (int y = 0; y < hh; ++y) {
        uint8_t* row = f->data[2] + y * f->linesize[2];
        for (int x = 0; x < hw; ++x)
            row[x] = static_cast<uint8_t>((y * 2) & 0xff);
    }

    return f;
}
