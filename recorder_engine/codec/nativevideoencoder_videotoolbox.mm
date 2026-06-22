#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/codec/avcc.h"

#ifdef __APPLE__

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <QList>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

struct EncodedPacket {
    QByteArray data;
    int64_t ptsTicks = 0;
    bool keyframe = false;
};

// Copy a CPU YUV420P AVFrame into an I420 CVPixelBuffer.
CVPixelBufferRef makeI420PixelBuffer(const AVFrame* f) {
    CVPixelBufferRef pb = nullptr;
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* vals[] = {(__bridge const void*)@{}};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, f->width, f->height,
                                           kCVPixelFormatType_420YpCbCr8Planar, attrs, &pb);
    if (attrs) CFRelease(attrs);
    if (r != kCVReturnSuccess || !pb) return nullptr;

    CVPixelBufferLockBaseAddress(pb, 0);
    for (int plane = 0; plane < 3; ++plane) {
        auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, plane));
        const size_t dstStride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
        const int rows = plane == 0 ? f->height : f->height / 2;
        const int bytes = plane == 0 ? f->width : f->width / 2;
        const uint8_t* src = f->data[plane];
        const int srcStride = f->linesize[plane];
        for (int y = 0; y < rows; ++y)
            memcpy(dst + y * dstStride, src + y * srcStride, bytes);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

QByteArray extractAvcC(CMSampleBufferRef sample) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    if (!fmt) return {};
    size_t count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr) != noErr)
        return {};
    QList<QByteArray> sps, pps;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* ps = nullptr;
        size_t psSize = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &psSize, nullptr, nullptr) != noErr)
            continue;
        const QByteArray nal(reinterpret_cast<const char*>(ps), int(psSize));
        const int nalType = psSize > 0 ? (ps[0] & 0x1f) : 0;
        if (nalType == 7) sps.append(nal);       // SPS
        else if (nalType == 8) pps.append(nal);  // PPS
    }
    return buildAvcCFromParameterSets(sps, pps);
}

bool sampleIsKeyframe(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return true; // no attachments → sync
    CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFBooleanRef notSync = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, (const void**)&notSync) && notSync)
        return !CFBooleanGetValue(notSync);
    return true;
}

// CMBlockBuffer for H.264 is length-prefixed (AVCC) — exactly what MKV wants.
QByteArray copyBlockBuffer(CMSampleBufferRef sample) {
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sample);
    if (!bb) return {};
    size_t total = CMBlockBufferGetDataLength(bb);
    QByteArray out(int(total), Qt::Uninitialized);
    if (CMBlockBufferCopyDataBytes(bb, 0, total, out.data()) != kCMBlockBufferNoErr) return {};
    return out;
}

} // namespace

class VideoToolboxEncoder : public NativeVideoEncoder {
public:
    VTCompressionSessionRef session = nullptr;
    QByteArray avcc;
    std::vector<EncodedPacket>* sink = nullptr; // set per encode call

    ~VideoToolboxEncoder() override {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    bool encode(const AVFrame* frame, int64_t ptsTicks,
                const PacketCallback& onPacket, QString* error) override {
        std::vector<EncodedPacket> packets;
        sink = &packets;
        CVPixelBufferRef pb = makeI420PixelBuffer(frame);
        if (!pb) { if (error) *error = QStringLiteral("CVPixelBuffer alloc failed"); sink = nullptr; return false; }

        const CMTime pts = CMTimeMake(ptsTicks, 90000);
        const void* fk[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* fv[] = {kCFBooleanTrue};
        CFDictionaryRef frameProps = CFDictionaryCreate(kCFAllocatorDefault, fk, fv, 1,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        const OSStatus st = VTCompressionSessionEncodeFrame(
            session, pb, pts, kCMTimeInvalid, frameProps,
            reinterpret_cast<void*>(ptsTicks), nullptr);
        if (frameProps) CFRelease(frameProps);
        CVPixelBufferRelease(pb);
        if (st != noErr) { if (error) *error = QStringLiteral("VTCompressionSessionEncodeFrame failed (%1)").arg(st); sink = nullptr; return false; }
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        sink = nullptr;
        for (auto& p : packets) onPacket(p.data, p.ptsTicks, p.keyframe);
        return true;
    }

    bool flush(const PacketCallback&, QString*) override {
        // Synchronous CompleteFrames in encode() means nothing is buffered.
        return true;
    }

    QByteArray avccExtradata() const override { return avcc; }
};

static void compressionOutputCallback(void* outputRefCon, void* sourceFrameRefCon,
                                      OSStatus status, VTEncodeInfoFlags,
                                      CMSampleBufferRef sample) {
    auto* self = static_cast<VideoToolboxEncoder*>(outputRefCon);
    if (status != noErr || !sample || !self || !self->sink) return;
    if (self->avcc.isEmpty()) self->avcc = extractAvcC(sample);
    EncodedPacket p;
    p.data = copyBlockBuffer(sample);
    p.ptsTicks = reinterpret_cast<int64_t>(sourceFrameRefCon);
    p.keyframe = sampleIsKeyframe(sample);
    if (!p.data.isEmpty()) self->sink->push_back(std::move(p));
}

NativeVideoEncoder::~NativeVideoEncoder() = default;

std::unique_ptr<NativeVideoEncoder> NativeVideoEncoder::create(const Config& cfg, QString* error) {
    auto enc = std::unique_ptr<VideoToolboxEncoder>(new VideoToolboxEncoder());
    CFDictionaryRef spec = nullptr;
    if (@available(iOS 17.4, tvOS 17.4, visionOS 1.1, *)) {
        const void* ek[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
        const void* ev[] = {kCFBooleanTrue};
        spec = CFDictionaryCreate(kCFAllocatorDefault, ek, ev, 1,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
    }
    OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, cfg.width, cfg.height,
                                             kCMVideoCodecType_H264, spec, nullptr, nullptr,
                                             compressionOutputCallback, enc.get(), &enc->session);
    if (spec) CFRelease(spec);
    if (st != noErr || !enc->session) {
        if (error) *error = QStringLiteral("VTCompressionSessionCreate failed (%1)").arg(st);
        return nullptr;
    }
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    const int one = 1;
    CFNumberRef kfi = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfi);
    if (kfi) CFRelease(kfi);
    CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &cfg.bitrate);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AverageBitRate, br);
    if (br) CFRelease(br);
    VTCompressionSessionPrepareToEncodeFrames(enc->session);
    return enc;
}

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities() {
    NativeVideoEncodeCapabilities caps;
    QString err;
    auto probe = NativeVideoEncoder::create({1280, 720, 30, 1, 4'000'000}, &err);
    caps.h264 = probe != nullptr;
    caps.detail = caps.h264 ? QStringLiteral("VideoToolbox H.264 encode available")
                            : QStringLiteral("VideoToolbox H.264 encode unavailable: %1").arg(err);
    return caps;
}

#endif // __APPLE__
