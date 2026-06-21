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
    using KeepSurfaceCallback = std::function<void(void* nativeDecodedImage, qint64 pts90k)>;

    NativeVideoDecoder(int outputWidth, int outputHeight);
    ~NativeVideoDecoder();

    NativeVideoDecoder(const NativeVideoDecoder&) = delete;
    NativeVideoDecoder& operator=(const NativeVideoDecoder&) = delete;

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
#if defined(__APPLE__) || defined(_WIN32)
    bool decodeKeepSurface(const CompressedAccessUnit& unit, KeepSurfaceCallback onSurface,
                           QString* error);
#else
    bool decodeKeepSurface(const CompressedAccessUnit&, KeepSurfaceCallback, QString* error) {
        if (error) {
            *error = QStringLiteral("Native keep-surface decode is unavailable on this platform");
        }
        return false;
    }
#endif
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
