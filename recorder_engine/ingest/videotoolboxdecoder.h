#ifndef VIDEOTOOLBOXDECODER_H
#define VIDEOTOOLBOXDECODER_H

#include "h26xaccessunit.h"

#include <QString>

#include <functional>

extern "C" {
#include <libavutil/frame.h>
}

class VideoToolboxDecoder {
public:
    using FrameCallback = std::function<void(AVFrame*)>;

    VideoToolboxDecoder(int outputWidth, int outputHeight);
    ~VideoToolboxDecoder();

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    void reset();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

#endif // VIDEOTOOLBOXDECODER_H
