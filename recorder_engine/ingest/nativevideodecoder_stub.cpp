#include "nativevideodecoder.h"

#ifndef __APPLE__
class NativeVideoDecoder::Impl {
public:
    Impl(int, int) {}
};

NativeVideoDecoder::NativeVideoDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

NativeVideoDecoder::~NativeVideoDecoder() {
    delete m_impl;
}

bool NativeVideoDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString* error) {
    if (error) {
        *error = QStringLiteral("Native video decode is unavailable on this platform");
    }
    return false;
}

void NativeVideoDecoder::reset() {}

bool NativeVideoDecoder::lastDecodedWasIOSurfaceBacked() const {
    return false;
}

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities() {
    NativeVideoDecodeCapabilities caps;
    caps.detail = QStringLiteral("Native video decode is unavailable on this platform");
    return caps;
}
#endif
