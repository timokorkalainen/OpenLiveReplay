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

// Media Foundation and VideoToolbox may return a previously submitted frame from
// the decode call for a newer access unit. Translate the output sample's real PTS
// relative to the submitted unit instead of stamping it with the newer unit's
// recording time. The signed delta is normalized across MPEG-TS's 33-bit wrap.
inline qint64 nativeVideoDecodedPtsDelta90k(qint64 submittedPts90k, qint64 decodedPts90k,
                                            qint64 wrap90k = qint64(1) << 33) {
    if (submittedPts90k < 0 || decodedPts90k < 0) {
        return 0;
    }
    qint64 delta90k = decodedPts90k - submittedPts90k;
    const qint64 halfWrap90k = wrap90k / 2;
    if (wrap90k > 0 && delta90k > halfWrap90k) {
        delta90k -= wrap90k;
    } else if (wrap90k > 0 && delta90k < -halfWrap90k) {
        delta90k += wrap90k;
    }
    return delta90k;
}

inline qint64 nativeVideoDecodedSourcePtsMs(qint64 submittedPts90k, qint64 submittedSourcePtsMs,
                                            qint64 decodedPts90k,
                                            qint64 wrap90k = qint64(1) << 33) {
    if (submittedSourcePtsMs < 0) {
        return submittedSourcePtsMs;
    }
    return submittedSourcePtsMs +
           nativeVideoDecodedPtsDelta90k(submittedPts90k, decodedPts90k, wrap90k) / 90;
}

inline qint64 nativeVideoDecodedTimecode100ns(qint64 submittedPts90k, qint64 submittedTimecode100ns,
                                              qint64 decodedPts90k,
                                              qint64 wrap90k = qint64(1) << 33) {
    if (submittedTimecode100ns < 0) {
        return submittedTimecode100ns;
    }
    constexpr qint64 day100ns = 24LL * 60 * 60 * 10000000;
    qint64 decodedTimecode100ns =
        submittedTimecode100ns +
        nativeVideoDecodedPtsDelta90k(submittedPts90k, decodedPts90k, wrap90k) * 10000000 / 90000;
    decodedTimecode100ns %= day100ns;
    if (decodedTimecode100ns < 0) {
        decodedTimecode100ns += day100ns;
    }
    return decodedTimecode100ns;
}

class NativeVideoDecoder {
public:
    using FrameCallback = std::function<void(AVFrame*)>;
    using KeepSurfaceCallback = std::function<bool(void* nativeDecodedImage, qint64 pts90k)>;

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
    void flushExcessPixelBufferPool();
    // Phase-0 probe (P0.1): true iff the most recently decoded CVPixelBuffer was
    // IOSurface-backed. Always false on non-VideoToolbox builds.
    bool lastDecodedWasIOSurfaceBacked() const;

private:
#if defined(OLR_UNIT_TEST) && defined(_WIN32)
    friend bool nativeVideoDecoderMediaFoundationDeliverOutputForTest(
        qint64 samplePts90k, qint64 fallbackPts90k, NativeVideoDecoder::FrameCallback onFrame,
        NativeVideoDecoder::KeepSurfaceCallback onSurface, QString* error);
#endif
    class Impl;
    Impl* m_impl = nullptr;
};

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities();

#ifdef OLR_UNIT_TEST
bool nativeVideoDecoderKeepSurfaceNullImageRejectedForTest();
bool nativeVideoDecoderNoFrameRejectedForTest(QString* error = nullptr);
#if defined(_WIN32)
QByteArray nativeVideoDecoderInputBytesForTest(const CompressedAccessUnit& unit,
                                               bool prependParameterSets);
bool nativeVideoDecoderMediaFoundationDeliverOutputForTest(
    qint64 samplePts90k, qint64 fallbackPts90k, NativeVideoDecoder::FrameCallback onFrame,
    NativeVideoDecoder::KeepSurfaceCallback onSurface, QString* error = nullptr);
#endif
#endif

#endif // NATIVEVIDEODECODER_H
