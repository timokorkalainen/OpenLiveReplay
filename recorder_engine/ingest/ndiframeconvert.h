#ifndef NDIFRAMECONVERT_H
#define NDIFRAMECONVERT_H

#include <QByteArray>

#include <cstdint>

extern "C" {
struct AVFrame;
struct SwsContext;
}

constexpr uint32_t ndiFourCc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
           (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

constexpr uint32_t kNdiFourCcUyvy = ndiFourCc('U', 'Y', 'V', 'Y');
constexpr uint32_t kNdiFourCcBgra = ndiFourCc('B', 'G', 'R', 'A');
constexpr uint32_t kNdiFourCcI420 = ndiFourCc('I', '4', '2', '0');

struct NdiVideoFrame {
    int width = 0;
    int height = 0;
    int strideBytes = 0;
    uint32_t fourCc = 0;
    const uint8_t* data = nullptr;
    int64_t timestamp100ns = 0;
    int64_t timecode100ns = 0;
};

struct NdiAudioFrame {
    int sampleRate = 48000;
    int channels = 2;
    int samples = 0;
    int channelStrideBytes = 0;
    const float* data = nullptr;
    int64_t timestamp100ns = 0;
    int64_t timecode100ns = 0;
};

AVFrame* ndiVideoToYuv420p(const NdiVideoFrame& in, int outWidth, int outHeight,
                           SwsContext** cache);
QByteArray ndiAudioToS16Stereo(const NdiAudioFrame& in);

#endif // NDIFRAMECONVERT_H
