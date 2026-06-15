#include "videotoolboxdecoder.h"

#ifndef __APPLE__
class VideoToolboxDecoder::Impl {
public:
    Impl(int, int) {}
};

VideoToolboxDecoder::VideoToolboxDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

VideoToolboxDecoder::~VideoToolboxDecoder() {
    delete m_impl;
}

bool VideoToolboxDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString* error) {
    if (error) {
        *error = QStringLiteral("VideoToolbox is unavailable on this platform");
    }
    return false;
}

void VideoToolboxDecoder::reset() {}
#endif
