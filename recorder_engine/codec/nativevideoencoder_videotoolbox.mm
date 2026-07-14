#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/codec/avcc.h"
#include "recorder_engine/codec/colorvui.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"

#ifdef __APPLE__

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
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

class RbspBitReader {
public:
    explicit RbspBitReader(const QByteArray& rbsp) : m_rbsp(rbsp) {}

    int bit() {
        if (m_bitOffset >= m_rbsp.size() * 8) {
            m_overrun = true;
            return 0;
        }
        const auto* data = reinterpret_cast<const quint8*>(m_rbsp.constData());
        const int value = (data[m_bitOffset / 8] >> (7 - (m_bitOffset % 8))) & 1;
        ++m_bitOffset;
        return value;
    }

    unsigned bits(int n) {
        unsigned value = 0;
        for (int i = 0; i < n; ++i)
            value = (value << 1) | unsigned(bit());
        return value;
    }

    unsigned ue() {
        int zeros = 0;
        while (!m_overrun && bit() == 0 && zeros < 32)
            ++zeros;
        if (zeros >= 32) {
            m_overrun = true;
            return 0;
        }
        return (1u << zeros) - 1u + bits(zeros);
    }

    int se() {
        const unsigned code = ue();
        return (code & 1u) ? int((code + 1u) / 2u) : -int(code / 2u);
    }

    int bitOffset() const { return m_bitOffset; }
    bool overrun() const { return m_overrun; }

private:
    const QByteArray& m_rbsp;
    int m_bitOffset = 8;
    bool m_overrun = false;
};

struct SpsColorBitOffsets {
    int fullRange = -1;
    int primaries = -1;
    int transfer = -1;
    int matrix = -1;
};

QByteArray rbspFromNal(const QByteArray& nal) {
    if (nal.isEmpty()) return {};
    QByteArray rbsp;
    rbsp.reserve(nal.size());
    rbsp.append(nal.at(0));
    int zeroes = 0;
    for (int i = 1; i < nal.size(); ++i) {
        const quint8 byte = quint8(nal.at(i));
        if (zeroes >= 2 && byte == 0x03) {
            zeroes = 0;
            continue;
        }
        rbsp.append(char(byte));
        if (byte == 0x00)
            ++zeroes;
        else
            zeroes = 0;
    }
    return rbsp;
}

QByteArray nalFromRbsp(const QByteArray& rbsp) {
    if (rbsp.isEmpty()) return {};
    QByteArray nal;
    nal.reserve(rbsp.size() + rbsp.size() / 32);
    nal.append(rbsp.at(0));
    int zeroes = 0;
    for (int i = 1; i < rbsp.size(); ++i) {
        const quint8 byte = quint8(rbsp.at(i));
        if (zeroes >= 2 && byte <= 0x03) {
            nal.append(char(0x03));
            zeroes = 0;
        }
        nal.append(char(byte));
        if (byte == 0x00)
            ++zeroes;
        else
            zeroes = 0;
    }
    return nal;
}

void writeBits(QByteArray* rbsp, int bitOffset, unsigned value, int bitCount) {
    auto* data = reinterpret_cast<quint8*>(rbsp->data());
    for (int i = 0; i < bitCount; ++i) {
        const unsigned bit = (value >> (bitCount - 1 - i)) & 1u;
        const int pos = bitOffset + i;
        quint8& byte = data[pos / 8];
        const quint8 mask = quint8(1u << (7 - (pos % 8)));
        if (bit)
            byte = quint8(byte | mask);
        else
            byte = quint8(byte & ~mask);
    }
}

bool hasHighProfileFields(unsigned profileIdc) {
    switch (profileIdc) {
    case 44:
    case 83:
    case 86:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 134:
    case 138:
    case 139:
    case 244:
        return true;
    default:
        return false;
    }
}

void skipScalingList(RbspBitReader* r, int size) {
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size && !r->overrun(); ++i) {
        if (nextScale != 0) {
            nextScale = (lastScale + r->se() + 256) % 256;
        }
        lastScale = nextScale == 0 ? lastScale : nextScale;
    }
}

bool findSpsColorBitOffsets(const QByteArray& rbsp, SpsColorBitOffsets* offsets) {
    if (!offsets || rbsp.size() < 4 || (quint8(rbsp.at(0)) & 0x1f) != 7) return false;

    RbspBitReader r(rbsp);
    const unsigned profileIdc = r.bits(8);
    r.bits(8);
    r.bits(8);
    r.ue();
    if (hasHighProfileFields(profileIdc)) {
        const unsigned chromaFormatIdc = r.ue();
        if (chromaFormatIdc == 3) r.bit();
        r.ue();
        r.ue();
        r.bit();
        if (r.bit()) {
            const int count = chromaFormatIdc != 3 ? 8 : 12;
            for (int i = 0; i < count && !r.overrun(); ++i) {
                if (r.bit()) skipScalingList(&r, i < 6 ? 16 : 64);
            }
        }
    }

    r.ue();
    const unsigned picOrderCntType = r.ue();
    if (picOrderCntType == 0) {
        r.ue();
    } else if (picOrderCntType == 1) {
        r.bit();
        r.se();
        r.se();
        const unsigned count = r.ue();
        for (unsigned i = 0; i < count && !r.overrun(); ++i)
            r.se();
    }
    r.ue();
    r.bit();
    r.ue();
    r.ue();
    const unsigned frameMbsOnly = r.bit();
    if (!frameMbsOnly) r.bit();
    r.bit();
    if (r.bit()) {
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    if (!r.bit() || r.overrun()) return false;

    if (r.bit()) {
        const unsigned aspectRatioIdc = r.bits(8);
        if (aspectRatioIdc == 255) {
            r.bits(16);
            r.bits(16);
        }
    }
    if (r.bit()) r.bit();
    if (!r.bit()) return false;
    r.bits(3);
    offsets->fullRange = r.bitOffset();
    r.bit();
    if (!r.bit()) return false;
    offsets->primaries = r.bitOffset();
    r.bits(8);
    offsets->transfer = r.bitOffset();
    r.bits(8);
    offsets->matrix = r.bitOffset();
    r.bits(8);
    return !r.overrun();
}

QByteArray rewriteSpsColorVui(const QByteArray& sps, const VuiColorCodePoints& vui) {
    QByteArray rbsp = rbspFromNal(sps);
    SpsColorBitOffsets offsets;
    if (!findSpsColorBitOffsets(rbsp, &offsets)) return sps;
    writeBits(&rbsp, offsets.fullRange, vui.fullRange ? 1u : 0u, 1);
    writeBits(&rbsp, offsets.primaries, unsigned(vui.colourPrimaries), 8);
    writeBits(&rbsp, offsets.transfer, unsigned(vui.transferCharacteristics), 8);
    writeBits(&rbsp, offsets.matrix, unsigned(vui.matrixCoefficients), 8);
    return nalFromRbsp(rbsp);
}

QByteArray rewriteAvcCColorVui(const QByteArray& avcc, const VuiColorCodePoints& vui) {
    QList<QByteArray> sps;
    QList<QByteArray> pps;
    if (!parseAvcc(avcc, &sps, &pps)) return avcc;
    // VideoToolbox may normalize semantically-equivalent transfer functions
    // (notably BT.601/SMPTE170M) in the emitted format description. The muxer
    // publishes avcC as codec private data, so patch the existing SPS VUI bits
    // to the exact H.273 code points requested by Config::color.
    for (QByteArray& nal : sps)
        nal = rewriteSpsColorVui(nal, vui);
    const QByteArray rewritten = buildAvcCFromParameterSets(sps, pps);
    return rewritten.isEmpty() ? avcc : rewritten;
}

CFStringRef cvPrimariesFor(int code) {
    switch (code) {
    case 9:
        return kCVImageBufferColorPrimaries_ITU_R_2020;
    case 6:
        return kCVImageBufferColorPrimaries_SMPTE_C;
    default:
        return kCVImageBufferColorPrimaries_ITU_R_709_2;
    }
}

CFStringRef cvTransferForSource(int code) {
    switch (code) {
    case 14:
        return kCVImageBufferTransferFunction_ITU_R_2020;
    case 6:
        return CFSTR("SMPTE_C");
    default:
        return kCVImageBufferTransferFunction_ITU_R_709_2;
    }
}

CFStringRef cvTransferForSession(int code) {
    switch (code) {
    case 14:
        return kCVImageBufferTransferFunction_ITU_R_2020;
    default:
        return kCVImageBufferTransferFunction_ITU_R_709_2;
    }
}

CFStringRef cvMatrixFor(int code) {
    switch (code) {
    case 9:
        return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    case 6:
        return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    default:
        return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    }
}

void attachColorMetadata(CVPixelBufferRef pb, const VuiColorCodePoints& vui) {
    CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, cvPrimariesFor(vui.colourPrimaries),
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey,
                          cvTransferForSource(vui.transferCharacteristics),
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, cvMatrixFor(vui.matrixCoefficients),
                          kCVAttachmentMode_ShouldPropagate);
}

// Copy a CPU YUV420P AVFrame into an I420 CVPixelBuffer.
CVPixelBufferRef makeI420PixelBuffer(const AVFrame* f, const VuiColorCodePoints& vui) {
    CVPixelBufferRef pb = nullptr;
    const OSType pixelFormat = vui.fullRange ? kCVPixelFormatType_420YpCbCr8PlanarFullRange
                                             : kCVPixelFormatType_420YpCbCr8Planar;
    const CVReturn r =
        CVPixelBufferCreate(kCFAllocatorDefault, f->width, f->height, pixelFormat, nullptr, &pb);
    if (r != kCVReturnSuccess || !pb) return nullptr;
    attachColorMetadata(pb, vui);

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

QByteArray extractAvcC(CMSampleBufferRef sample, const VuiColorCodePoints& vui) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    if (!fmt) return {};
    size_t count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count,
                                                           nullptr) != noErr)
        return {};
    QList<QByteArray> sps, pps;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* ps = nullptr;
        size_t psSize = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &psSize, nullptr,
                                                               nullptr) != noErr)
            continue;
        const QByteArray nal(reinterpret_cast<const char*>(ps), int(psSize));
        const int nalType = psSize > 0 ? (ps[0] & 0x1f) : 0;
        if (nalType == 7)
            sps.append(nal); // SPS
        else if (nalType == 8)
            pps.append(nal); // PPS
    }
    const QByteArray avcc = buildAvcCFromParameterSets(sps, pps);
    return rewriteAvcCColorVui(avcc, vui);
}

bool sampleIsKeyframe(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return true; // no attachments → sync
    CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFBooleanRef notSync = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync,
                                      (const void**)&notSync) &&
        notSync)
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
    VuiColorCodePoints vui;
    bool requestedFullRange = false;
    std::vector<EncodedPacket>* sink = nullptr; // set per encode call

    ~VideoToolboxEncoder() override {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    bool encode(const AVFrame* frame, int64_t ptsTicks, const PacketCallback& onPacket,
                QString* error) override {
        CVPixelBufferRef pb = makeI420PixelBuffer(frame, vui);
        if (!pb) {
            if (error) *error = QStringLiteral("CVPixelBuffer alloc failed");
            return false;
        }
        const bool ok = encodePixelBuffer(pb, ptsTicks, onPacket, error);
        CVPixelBufferRelease(pb);
        return ok;
    }

    bool encodeSurface(GpuSurface* surface, int64_t ptsTicks, const ColorMetadata& color,
                       const PacketCallback& onPacket, QString* error) override {
        if (!surface || !surface->isValid()) {
            if (error) *error = QStringLiteral("encodeSurface: null/invalid surface");
            return false;
        }
        GpuSyncReadScope readScope;
        const GpuReadLease lease = readScope.read(surface);
        const bool encoded = [&] {
            IOSurfaceRef ioSurface = static_cast<IOSurfaceRef>(lease.nativeHandle());
            if (!ioSurface) {
                if (error) {
                    *error = QStringLiteral("encodeSurface: surface is not IOSurface-backed");
                }
                return false;
            }
            const GpuSurfaceDesc desc = lease.desc();
            if (desc.format != FramePixelFormat::Nv12) {
                if (error) *error = QStringLiteral("encodeSurface: expected NV12 surface");
                return false;
            }
            if (desc.width > 0 && desc.height > 0 &&
                (desc.width != int(IOSurfaceGetWidth(ioSurface)) ||
                 desc.height != int(IOSurfaceGetHeight(ioSurface)))) {
                if (error)
                    *error = QStringLiteral("encodeSurface: descriptor/native size mismatch");
                return false;
            }

            CVPixelBufferRef pb = nullptr;
            const CVReturn rc =
                CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pb);
            if (rc != kCVReturnSuccess || !pb) {
                if (error) {
                    *error = QStringLiteral("CVPixelBufferCreateWithIOSurface failed (%1)").arg(rc);
                }
                return false;
            }
            attachColorMetadata(pb, vuiColorCodePointsFor(color));
            const bool ok = encodePixelBuffer(pb, ptsTicks, onPacket, error);
            CVPixelBufferRelease(pb);
            return ok;
        }();
        readScope.complete();
        return encoded;
    }

    bool encodePixelBuffer(CVPixelBufferRef pb, int64_t ptsTicks, const PacketCallback& onPacket,
                           QString* error) {
        std::vector<EncodedPacket> packets;
        sink = &packets;

        const CMTime pts = CMTimeMake(ptsTicks, 90000);
        const void* fk[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* fv[] = {kCFBooleanTrue};
        CFDictionaryRef frameProps =
            CFDictionaryCreate(kCFAllocatorDefault, fk, fv, 1, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        const OSStatus st =
            VTCompressionSessionEncodeFrame(session, pb, pts, kCMTimeInvalid, frameProps,
                                            reinterpret_cast<void*>(ptsTicks), nullptr);
        if (frameProps) CFRelease(frameProps);
        if (st != noErr) {
            if (error)
                *error = QStringLiteral("VTCompressionSessionEncodeFrame failed (%1)").arg(st);
            sink = nullptr;
            return false;
        }
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        sink = nullptr;
        for (auto& p : packets)
            onPacket(p.data, p.ptsTicks, p.keyframe);
        return true;
    }

    bool flush(const PacketCallback&, QString*) override {
        // Synchronous CompleteFrames in encode() means nothing is buffered.
        return true;
    }

    QByteArray avccExtradata() const override { return avcc; }
};

static void compressionOutputCallback(void* outputRefCon, void* sourceFrameRefCon, OSStatus status,
                                      VTEncodeInfoFlags, CMSampleBufferRef sample) {
    auto* self = static_cast<VideoToolboxEncoder*>(outputRefCon);
    if (status != noErr || !sample || !self || !self->sink) return;
    if (self->avcc.isEmpty()) self->avcc = extractAvcC(sample, self->vui);
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
    if (@available(macOS 10.9, iOS 17.4, tvOS 17.4, visionOS 1.1, *)) {
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
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_High_AutoLevel);
    const int one = 1;
    CFNumberRef kfi = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfi);
    if (kfi) CFRelease(kfi);
    CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &cfg.bitrate);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AverageBitRate, br);
    if (br) CFRelease(br);
    enc->vui = vuiColorCodePointsFor(cfg.color);
    enc->requestedFullRange = enc->vui.fullRange;
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ColorPrimaries,
                         cvPrimariesFor(enc->vui.colourPrimaries));
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_TransferFunction,
                         cvTransferForSession(enc->vui.transferCharacteristics));
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_YCbCrMatrix,
                         cvMatrixFor(enc->vui.matrixCoefficients));
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
