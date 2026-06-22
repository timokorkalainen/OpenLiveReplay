#include "nativevideodecoder.h"

#ifdef __APPLE__

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

struct AnnexBNal {
    int payloadOffset = 0;
    int endOffset = 0;
};

struct DecodeFrameContext {
    NativeVideoDecoder::FrameCallback* callback = nullptr;
    NativeVideoDecoder::KeepSurfaceCallback* surfaceCallback = nullptr;
    qint64 pts90k = -1;
    bool keepSurface = false;
    bool emittedFrame = false;
    bool copyFailed = false;
    bool ioSurfaceBacked = false;
    OSStatus callbackStatus = noErr;
};

int startCodeSizeAt(const QByteArray& bytes, int offset) {
    if (offset + 3 <= bytes.size()
        && bytes[offset] == char(0)
        && bytes[offset + 1] == char(0)
        && bytes[offset + 2] == char(1)) {
        return 3;
    }
    if (offset + 4 <= bytes.size()
        && bytes[offset] == char(0)
        && bytes[offset + 1] == char(0)
        && bytes[offset + 2] == char(0)
        && bytes[offset + 3] == char(1)) {
        return 4;
    }
    return 0;
}

QList<AnnexBNal> splitAnnexBNals(const QByteArray& bytes) {
    QList<int> starts;
    for (int i = 0; i + 3 <= bytes.size();) {
        const int prefixSize = startCodeSizeAt(bytes, i);
        if (prefixSize > 0) {
            starts.append(i);
            i += prefixSize;
        } else {
            ++i;
        }
    }

    QList<AnnexBNal> nals;
    for (int i = 0; i < starts.size(); ++i) {
        const int prefixSize = startCodeSizeAt(bytes, starts[i]);
        const int payloadOffset = starts[i] + prefixSize;
        const int endOffset =
            (i + 1 < starts.size()) ? starts[i + 1] : static_cast<int>(bytes.size());
        if (prefixSize > 0 && endOffset > payloadOffset) {
            nals.append({payloadOffset, endOffset});
        }
    }
    return nals;
}

void appendParameterSet(QByteArray* key, const QByteArray& nal) {
    if (!key) {
        return;
    }
    const quint32 size = quint32(nal.size());
    key->append(char((size >> 24) & 0xff));
    key->append(char((size >> 16) & 0xff));
    key->append(char((size >> 8) & 0xff));
    key->append(char(size & 0xff));
    key->append(nal);
}

QByteArray parameterSetKey(NativeVideoCodec codec, const H26xParameterSets& parameterSets) {
    QByteArray key;
    key.append(char(codec == NativeVideoCodec::H264 ? 1 : codec == NativeVideoCodec::Hevc ? 2 : 0));
    if (codec == NativeVideoCodec::H264) {
        for (const QByteArray& sps : parameterSets.h264Sps) appendParameterSet(&key, sps);
        key.append(char(0));
        for (const QByteArray& pps : parameterSets.h264Pps) appendParameterSet(&key, pps);
    } else if (codec == NativeVideoCodec::Hevc) {
        for (const QByteArray& vps : parameterSets.hevcVps) appendParameterSet(&key, vps);
        key.append(char(0));
        for (const QByteArray& sps : parameterSets.hevcSps) appendParameterSet(&key, sps);
        key.append(char(0));
        for (const QByteArray& pps : parameterSets.hevcPps) appendParameterSet(&key, pps);
    }
    return key.size() > 1 ? key : QByteArray();
}

QByteArray annexBToLengthPrefixed(const QByteArray& annexB) {
    QByteArray sample;
    const QList<AnnexBNal> nals = splitAnnexBNals(annexB);
    sample.reserve(annexB.size());
    for (const AnnexBNal& nal : nals) {
        const quint32 size = quint32(nal.endOffset - nal.payloadOffset);
        sample.append(char((size >> 24) & 0xff));
        sample.append(char((size >> 16) & 0xff));
        sample.append(char((size >> 8) & 0xff));
        sample.append(char(size & 0xff));
        sample.append(annexB.constData() + nal.payloadOffset, int(size));
    }
    return sample;
}

QString statusMessage(const QString& action, OSStatus status) {
    return QStringLiteral("%1 (OSStatus %2)").arg(action).arg(status);
}

void copyRows(const uchar* src, size_t srcStride, uchar* dst, int dstStride,
              int bytesPerRow, int rows) {
    for (int y = 0; y < rows; ++y) {
        memcpy(dst + y * dstStride, src + y * srcStride, size_t(bytesPerRow));
    }
}

AVFrame* copyPixelBufferToAvFrame(CVPixelBufferRef pixelBuffer) {
    if (!pixelBuffer || !CVPixelBufferIsPlanar(pixelBuffer)) {
        return nullptr;
    }

    if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return nullptr;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        return nullptr;
    }

    const int width = int(CVPixelBufferGetWidth(pixelBuffer));
    const int height = int(CVPixelBufferGetHeight(pixelBuffer));
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        return nullptr;
    }

    const size_t planeCount = CVPixelBufferGetPlaneCount(pixelBuffer);
    if (planeCount == 2) {
        const auto* ySrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
        const auto* uvSrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));
        const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
        const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
        const int yRows = std::min(height, int(CVPixelBufferGetHeightOfPlane(pixelBuffer, 0)));
        const int uvRows = std::min(height / 2, int(CVPixelBufferGetHeightOfPlane(pixelBuffer, 1)));
        const int chromaWidth = width / 2;

        copyRows(ySrc, yStride, frame->data[0], frame->linesize[0], width, yRows);
        for (int y = 0; y < uvRows; ++y) {
            const uchar* srcRow = uvSrc + y * uvStride;
            uchar* uRow = frame->data[1] + y * frame->linesize[1];
            uchar* vRow = frame->data[2] + y * frame->linesize[2];
            for (int x = 0; x < chromaWidth; ++x) {
                uRow[x] = srcRow[x * 2];
                vRow[x] = srcRow[x * 2 + 1];
            }
        }
    } else if (planeCount == 3) {
        for (int plane = 0; plane < 3; ++plane) {
            const auto* src = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, plane));
            const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, plane);
            const int rows = std::min(plane == 0 ? height : height / 2,
                                      int(CVPixelBufferGetHeightOfPlane(pixelBuffer, plane)));
            const int bytesPerRow = plane == 0 ? width : width / 2;
            copyRows(src, stride, frame->data[plane], frame->linesize[plane], bytesPerRow, rows);
        }
    } else {
        av_frame_free(&frame);
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    return frame;
}

static void decompressionOutputCallback(void*,
                                        void* sourceFrameRefCon,
                                        OSStatus status,
                                        VTDecodeInfoFlags,
                                        CVImageBufferRef imageBuffer,
                                        CMTime,
                                        CMTime) {
    auto* context = static_cast<DecodeFrameContext*>(sourceFrameRefCon);
    if (!context) {
        return;
    }
    if (status != noErr) {
        context->callbackStatus = status;
        return;
    }
    if (!imageBuffer) {
        return;
    }

    context->ioSurfaceBacked =
        CVPixelBufferGetIOSurface(CVPixelBufferRef(imageBuffer)) != nullptr;

    if (context->keepSurface) {
        if (!context->surfaceCallback) {
            return;
        }
        context->emittedFrame = true;
        (*context->surfaceCallback)(imageBuffer, context->pts90k);
        return;
    }

    if (!context->callback) {
        return;
    }

    AVFrame* frame = copyPixelBufferToAvFrame(CVPixelBufferRef(imageBuffer));
    if (!frame) {
        context->copyFailed = true;
        return;
    }

    frame->pts = context->pts90k;
    context->emittedFrame = true;
    (*context->callback)(frame);
}

} // namespace

class NativeVideoDecoder::Impl {
public:
    Impl(int outputWidth, int outputHeight)
        : width(outputWidth)
        , height(outputHeight) {}

    ~Impl() { reset(); }

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    bool decodeKeepSurface(const CompressedAccessUnit& unit,
                           KeepSurfaceCallback onSurface,
                           QString* error);
    void reset();
    bool lastDecodedWasIOSurfaceBacked() const { return lastIOSurfaceBacked; }

private:
    int width = 0;
    int height = 0;
    NativeVideoCodec codec = NativeVideoCodec::Unknown;
    QByteArray activeParameterSetKey;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    bool lastIOSurfaceBacked = false;

    bool ensureSession(const CompressedAccessUnit& unit, QString* error);
    bool createFormatDescription(const CompressedAccessUnit& unit, QString* error);
    bool createSession(QString* error);
};

void NativeVideoDecoder::Impl::reset() {
    if (session) {
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        session = nullptr;
    }
    if (format) {
        CFRelease(format);
        format = nullptr;
    }
    codec = NativeVideoCodec::Unknown;
    activeParameterSetKey.clear();
}

bool NativeVideoDecoder::Impl::createFormatDescription(const CompressedAccessUnit& unit,
                                                        QString* error) {
    std::vector<const uint8_t*> pointers;
    std::vector<size_t> sizes;
    const auto append = [&](const QList<QByteArray>& nals) {
        for (const QByteArray& nal : nals) {
            if (!nal.isEmpty()) {
                pointers.push_back(reinterpret_cast<const uint8_t*>(nal.constData()));
                sizes.push_back(size_t(nal.size()));
            }
        }
    };

    OSStatus status = noErr;
    CMFormatDescriptionRef createdFormat = nullptr;
    if (unit.codec == NativeVideoCodec::H264) {
        if (unit.parameterSets.h264Sps.isEmpty() || unit.parameterSets.h264Pps.isEmpty()) {
            if (error) *error = QStringLiteral("VideoToolbox H.264 decode requires SPS and PPS");
            return false;
        }
        append(unit.parameterSets.h264Sps);
        append(unit.parameterSets.h264Pps);
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault,
            pointers.size(),
            pointers.data(),
            sizes.data(),
            4,
            &createdFormat);
    } else if (unit.codec == NativeVideoCodec::Hevc) {
        if (unit.parameterSets.hevcVps.isEmpty()
            || unit.parameterSets.hevcSps.isEmpty()
            || unit.parameterSets.hevcPps.isEmpty()) {
            if (error) *error = QStringLiteral("VideoToolbox HEVC decode requires VPS, SPS, and PPS");
            return false;
        }
        append(unit.parameterSets.hevcVps);
        append(unit.parameterSets.hevcSps);
        append(unit.parameterSets.hevcPps);
        status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            kCFAllocatorDefault,
            pointers.size(),
            pointers.data(),
            sizes.data(),
            4,
            nullptr,
            &createdFormat);
    } else {
        if (error) *error = QStringLiteral("VideoToolbox decode requires H.264 or HEVC access units");
        return false;
    }

    if (status != noErr || !createdFormat) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox format description creation failed"), status);
        return false;
    }

    format = (CMVideoFormatDescriptionRef)createdFormat;
    return true;
}

bool NativeVideoDecoder::Impl::createSession(QString* error) {
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSType pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pixelFormatNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, pixelFormatNumber);
    CFRelease(pixelFormatNumber);

    CFDictionaryRef ioSurfaceProps = CFDictionaryCreate(
        kCFAllocatorDefault, nullptr, nullptr, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, ioSurfaceProps);
    CFRelease(ioSurfaceProps);

    if (width > 0) {
        CFNumberRef widthNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
        CFDictionarySetValue(attributes, kCVPixelBufferWidthKey, widthNumber);
        CFRelease(widthNumber);
    }
    if (height > 0) {
        CFNumberRef heightNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
        CFDictionarySetValue(attributes, kCVPixelBufferHeightKey, heightNumber);
        CFRelease(heightNumber);
    }

    VTDecompressionOutputCallbackRecord callback;
    callback.decompressionOutputCallback = decompressionOutputCallback;
    callback.decompressionOutputRefCon = nullptr;
    const OSStatus status = VTDecompressionSessionCreate(
        kCFAllocatorDefault, format, nullptr, attributes, &callback, &session);
    CFRelease(attributes);

    if (status != noErr || !session) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox decompression session creation failed"), status);
        return false;
    }

    return true;
}

bool NativeVideoDecoder::Impl::ensureSession(const CompressedAccessUnit& unit, QString* error) {
    const QByteArray nextKey = parameterSetKey(unit.codec, unit.parameterSets);
    if (session && unit.codec == codec && (nextKey.isEmpty() || nextKey == activeParameterSetKey)) {
        return true;
    }
    if (nextKey.isEmpty()) {
        if (error) {
            *error = QStringLiteral("VideoToolbox needs codec parameter sets before the first frame");
        }
        return false;
    }

    reset();
    codec = unit.codec;
    activeParameterSetKey = nextKey;

    if (!createFormatDescription(unit, error) || !createSession(error)) {
        reset();
        return false;
    }
    return true;
}

bool NativeVideoDecoder::Impl::decode(const CompressedAccessUnit& unit,
                                       FrameCallback onFrame,
                                       QString* error) {
    if (!onFrame) {
        if (error) *error = QStringLiteral("VideoToolbox decode requires a frame callback");
        return false;
    }
    if (!ensureSession(unit, error)) {
        return false;
    }

    QByteArray sampleData = annexBToLengthPrefixed(unit.annexB);
    if (sampleData.isEmpty()) {
        if (error) *error = QStringLiteral("VideoToolbox decode received an empty access unit");
        return false;
    }

    CMBlockBufferRef blockBuffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        sampleData.data(),
        sampleData.size(),
        kCFAllocatorNull,
        nullptr,
        0,
        sampleData.size(),
        0,
        &blockBuffer);
    if (status != noErr || !blockBuffer) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox block buffer creation failed"), status);
        return false;
    }

    const size_t sampleSize = size_t(sampleData.size());
    const CMTime pts = unit.pts90k >= 0 ? CMTimeMake(unit.pts90k, 90000) : kCMTimeInvalid;
    const CMTime dts = unit.dts90k >= 0 ? CMTimeMake(unit.dts90k, 90000) : kCMTimeInvalid;
    CMSampleTimingInfo timing;
    timing.duration = kCMTimeInvalid;
    timing.presentationTimeStamp = pts;
    timing.decodeTimeStamp = dts;

    CMSampleBufferRef sampleBuffer = nullptr;
    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        blockBuffer,
        format,
        1,
        1,
        &timing,
        1,
        &sampleSize,
        &sampleBuffer);
    CFRelease(blockBuffer);
    if (status != noErr || !sampleBuffer) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox sample buffer creation failed"), status);
        return false;
    }

    DecodeFrameContext context;
    context.callback = &onFrame;
    context.pts90k = unit.pts90k;
    VTDecodeInfoFlags infoFlags = 0;
    status = VTDecompressionSessionDecodeFrame(session, sampleBuffer, 0, &context, &infoFlags);
    CFRelease(sampleBuffer);
    if (status != noErr) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox frame decode failed"), status);
        return false;
    }

    VTDecompressionSessionWaitForAsynchronousFrames(session);
    lastIOSurfaceBacked = context.ioSurfaceBacked;
    if (context.callbackStatus != noErr) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox output callback failed"), context.callbackStatus);
        return false;
    }
    if (context.copyFailed) {
        if (error) *error = QStringLiteral("VideoToolbox decoded a frame but pixel buffer copy failed");
        return false;
    }
    return true;
}

bool NativeVideoDecoder::Impl::decodeKeepSurface(const CompressedAccessUnit& unit,
                                                  KeepSurfaceCallback onSurface,
                                                  QString* error) {
    if (!onSurface) {
        if (error) *error = QStringLiteral("VideoToolbox keep-surface decode requires a callback");
        return false;
    }
    if (!ensureSession(unit, error)) {
        return false;
    }

    QByteArray sampleData = annexBToLengthPrefixed(unit.annexB);
    if (sampleData.isEmpty()) {
        if (error) *error = QStringLiteral("VideoToolbox decode received an empty access unit");
        return false;
    }

    CMBlockBufferRef blockBuffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        sampleData.data(),
        sampleData.size(),
        kCFAllocatorNull,
        nullptr,
        0,
        sampleData.size(),
        0,
        &blockBuffer);
    if (status != noErr || !blockBuffer) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox block buffer creation failed"), status);
        return false;
    }

    const size_t sampleSize = size_t(sampleData.size());
    const CMTime pts = unit.pts90k >= 0 ? CMTimeMake(unit.pts90k, 90000) : kCMTimeInvalid;
    const CMTime dts = unit.dts90k >= 0 ? CMTimeMake(unit.dts90k, 90000) : kCMTimeInvalid;
    CMSampleTimingInfo timing;
    timing.duration = kCMTimeInvalid;
    timing.presentationTimeStamp = pts;
    timing.decodeTimeStamp = dts;

    CMSampleBufferRef sampleBuffer = nullptr;
    status = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        blockBuffer,
        format,
        1,
        1,
        &timing,
        1,
        &sampleSize,
        &sampleBuffer);
    CFRelease(blockBuffer);
    if (status != noErr || !sampleBuffer) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox sample buffer creation failed"), status);
        return false;
    }

    DecodeFrameContext context;
    context.keepSurface = true;
    context.surfaceCallback = &onSurface;
    context.pts90k = unit.pts90k;
    VTDecodeInfoFlags infoFlags = 0;
    status = VTDecompressionSessionDecodeFrame(session, sampleBuffer, 0, &context, &infoFlags);
    CFRelease(sampleBuffer);
    if (status != noErr) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox frame decode failed"), status);
        return false;
    }

    VTDecompressionSessionWaitForAsynchronousFrames(session);
    lastIOSurfaceBacked = context.ioSurfaceBacked;
    if (context.callbackStatus != noErr) {
        if (error) *error = statusMessage(QStringLiteral("VideoToolbox output callback failed"), context.callbackStatus);
        return false;
    }
    return true;
}

NativeVideoDecoder::NativeVideoDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

NativeVideoDecoder::~NativeVideoDecoder() {
    delete m_impl;
}

bool NativeVideoDecoder::decode(const CompressedAccessUnit& unit,
                                 FrameCallback onFrame,
                                 QString* error) {
    return m_impl->decode(unit, std::move(onFrame), error);
}

bool NativeVideoDecoder::decodeKeepSurface(const CompressedAccessUnit& unit,
                                           KeepSurfaceCallback onSurface,
                                           QString* error) {
    return m_impl->decodeKeepSurface(unit, std::move(onSurface), error);
}

void NativeVideoDecoder::reset() {
    m_impl->reset();
}

bool NativeVideoDecoder::lastDecodedWasIOSurfaceBacked() const {
    return m_impl->lastDecodedWasIOSurfaceBacked();
}

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities() {
    NativeVideoDecodeCapabilities caps;
    caps.h264 = true;
    caps.hevc = true;
    caps.detail = QStringLiteral("VideoToolbox native decode available");
    return caps;
}

#endif
