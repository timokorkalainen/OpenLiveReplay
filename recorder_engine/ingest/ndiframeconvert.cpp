#include "ndiframeconvert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

bool validEvenSize(int width, int height) {
    return width > 0 && height > 0 && (width % 2) == 0 && (height % 2) == 0;
}

void copyRows(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride, int bytesPerRow,
              int rows) {
    for (int y = 0; y < rows; ++y) {
        memcpy(dst + y * dstStride, src + y * srcStride, size_t(bytesPerRow));
    }
}

AVFrame* allocYuv420p(int width, int height) {
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
    return frame;
}

AVPixelFormat pixFmtForFourCc(uint32_t fourCc) {
    if (fourCc == kNdiFourCcUyvy) return AV_PIX_FMT_UYVY422;
    if (fourCc == kNdiFourCcBgra) return AV_PIX_FMT_BGRA;
    if (fourCc == kNdiFourCcI420) return AV_PIX_FMT_YUV420P;
    return AV_PIX_FMT_NONE;
}

int16_t floatToS16(float sample) {
    const float clamped = std::max(-1.0f, std::min(1.0f, sample));
    const int value = int(std::lrintf(clamped * 32768.0f));
    return int16_t(std::max(-32768, std::min(32767, value)));
}

float planarSample(const NdiAudioFrame& in, int channel, int sample) {
    const int srcChannel = std::min(channel, std::max(0, in.channels - 1));
    const auto* bytes = reinterpret_cast<const uint8_t*>(in.data);
    const auto* plane = reinterpret_cast<const float*>(bytes + srcChannel * in.channelStrideBytes);
    return plane[std::max(0, std::min(sample, in.samples - 1))];
}

} // namespace

AVFrame* ndiVideoToYuv420p(const NdiVideoFrame& in, int outWidth, int outHeight,
                           SwsContext** cache) {
    if (!in.data || !validEvenSize(in.width, in.height) || !validEvenSize(outWidth, outHeight) ||
        in.strideBytes <= 0) {
        return nullptr;
    }

    AVFrame* out = allocYuv420p(outWidth, outHeight);
    if (!out) {
        return nullptr;
    }

    if (in.fourCc == kNdiFourCcI420 && in.width == outWidth && in.height == outHeight) {
        if (in.strideBytes < in.width) {
            av_frame_free(&out);
            return nullptr;
        }
        const int chromaStride = in.strideBytes / 2;
        const uint8_t* y = in.data;
        const uint8_t* u = y + in.strideBytes * in.height;
        const uint8_t* v = u + chromaStride * (in.height / 2);
        copyRows(y, in.strideBytes, out->data[0], out->linesize[0], in.width, in.height);
        copyRows(u, chromaStride, out->data[1], out->linesize[1], in.width / 2, in.height / 2);
        copyRows(v, chromaStride, out->data[2], out->linesize[2], in.width / 2, in.height / 2);
        return out;
    }

    const AVPixelFormat srcFormat = pixFmtForFourCc(in.fourCc);
    if (srcFormat == AV_PIX_FMT_NONE || !cache) {
        av_frame_free(&out);
        return nullptr;
    }
    *cache = sws_getCachedContext(*cache, in.width, in.height, srcFormat, outWidth, outHeight,
                                  AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!*cache) {
        av_frame_free(&out);
        return nullptr;
    }

    const uint8_t* srcData[4] = {in.data, nullptr, nullptr, nullptr};
    int srcStride[4] = {in.strideBytes, 0, 0, 0};
    if (sws_scale(*cache, srcData, srcStride, 0, in.height, out->data, out->linesize) <= 0) {
        av_frame_free(&out);
        return nullptr;
    }
    return out;
}

QByteArray ndiAudioToS16Stereo(const NdiAudioFrame& in) {
    if (!in.data || in.sampleRate <= 0 || in.channels <= 0 || in.samples <= 0 ||
        in.channelStrideBytes < int(sizeof(float)) * in.samples) {
        return {};
    }

    const int outSamples =
        std::max(1, int(std::llround(double(in.samples) * 48000.0 / double(in.sampleRate))));
    QByteArray out(outSamples * 2 * int(sizeof(int16_t)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(out.data());
    for (int i = 0; i < outSamples; ++i) {
        const double srcPos = double(i) * double(in.sampleRate) / 48000.0;
        const int a = std::min(in.samples - 1, int(std::floor(srcPos)));
        const int b = std::min(in.samples - 1, a + 1);
        const float frac = float(srcPos - double(a));
        for (int ch = 0; ch < 2; ++ch) {
            const int srcCh = in.channels == 1 ? 0 : ch;
            const float s0 = planarSample(in, srcCh, a);
            const float s1 = planarSample(in, srcCh, b);
            dst[i * 2 + ch] = floatToS16(s0 + (s1 - s0) * frac);
        }
    }
    return out;
}
