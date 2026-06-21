#ifndef NATIVEVIDEODECODER_H
#define NATIVEVIDEODECODER_H

#include "h26xaccessunit.h"

#include <QString>

#include <functional>

extern "C" {
struct AVFrame;
}

struct NativeVideoDecodeCapabilities {
    bool h264 = false;
    bool hevc = false;
    bool d3d11 = false;
    QString detail;
};

class NativeVideoDecoder {
public:
    using FrameCallback = std::function<void(AVFrame*)>;

    NativeVideoDecoder(int outputWidth, int outputHeight);
    ~NativeVideoDecoder();

    NativeVideoDecoder(const NativeVideoDecoder&) = delete;
    NativeVideoDecoder& operator=(const NativeVideoDecoder&) = delete;

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    void reset();
    // Phase-0 probe (P0.1): true iff the most recently decoded CVPixelBuffer was
    // IOSurface-backed. Always false on non-VideoToolbox builds.
    bool lastDecodedWasIOSurfaceBacked() const;

private:
    class Impl;
    Impl* m_impl = nullptr;
};

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities();

#endif // NATIVEVIDEODECODER_H
