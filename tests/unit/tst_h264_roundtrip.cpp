// End-to-end avcC + muxing round-trip: native-encode a textured pattern, attach the
// encoder's avcC to the muxer, write a real MKV, then demux it and assert the
// stream is H.264, every frame is a keyframe, and the frame count matches.
// Then decode back via NativeVideoDecoder and assert frame dimensions AND that the
// decoded luma matches the source within a PSNR floor (objective quality gate, T3.4).
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>

#include "recorder_engine/codec/avcc.h"
#include "recorder_engine/codec/colorvui.h"
#include "recorder_engine/muxer.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/ingest/nativevideodecoder.h"
#include "tests/unit/framepsnr.h"
#if defined(__APPLE__) && defined(OLR_GPU_PIPELINE_BUILD)
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/vtkeepsurfaceimporter.h"
#endif

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

namespace {

AVFrame* makePattern() {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = 640;
    f->height = 480;
    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    for (int y = 0; y < 480; ++y) {
        uint8_t* row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < 640; ++x) {
            const int base = (x * 256 / 640 + y * 256 / 480) / 2;
            const int block = (((x >> 5) + (y >> 5)) & 1) ? 24 : -24;
            const unsigned h = unsigned(x) * 2654435761u + unsigned(y) * 40503u;
            const int hf = int((h >> 24) & 0x1f) - 16;
            const int v = base + block + hf;
            row[x] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < 240; ++y) {
        uint8_t* u = f->data[1] + y * f->linesize[1];
        uint8_t* vrow = f->data[2] + y * f->linesize[2];
        for (int x = 0; x < 320; ++x) {
            u[x] = uint8_t(96 + x * 64 / 320);
            vrow[x] = uint8_t(96 + y * 64 / 240);
        }
    }
    return f;
}

#if defined(__APPLE__) && defined(OLR_GPU_PIPELINE_BUILD)
QByteArray lengthPrefixedToAnnexB(const QByteArray& packet) {
    if (packet.size() >= 4 && packet[0] == '\0' && packet[1] == '\0' && packet[2] == '\0' &&
        packet[3] == '\1') {
        return packet;
    }

    QByteArray annexB;
    const auto* p = reinterpret_cast<const uint8_t*>(packet.constData());
    const uint8_t* end = p + packet.size();
    static const char kStartCode[4] = {'\0', '\0', '\0', '\1'};
    while (p + 4 <= end) {
        const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        p += 4;
        if (nalLen == 0 || p + nalLen > end ||
            nalLen > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            return QByteArray();
        }
        annexB.append(kStartCode, 4);
        annexB.append(reinterpret_cast<const char*>(p), static_cast<int>(nalLen));
        p += nalLen;
    }
    return p == end ? annexB : QByteArray();
}

bool parameterSetsFromAvcc(const QByteArray& avcc, H26xParameterSets* out) {
    if (!out) return false;
    QList<QByteArray> sps;
    QList<QByteArray> pps;
    if (!parseAvcc(avcc, &sps, &pps) || sps.isEmpty() || pps.isEmpty()) return false;
    out->h264Sps = sps;
    out->h264Pps = pps;
    return true;
}

CompressedAccessUnit makeH264Unit(const QByteArray& packet, const H26xParameterSets& ps) {
    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::H264;
    unit.parameterSets = ps;
    unit.pts90k = 0;
    unit.dts90k = 0;
    unit.annexB = lengthPrefixedToAnnexB(packet);
    return unit;
}

struct PlanePsnr {
    double y = 0.0;
    double u = 0.0;
    double v = 0.0;
};

std::optional<PlanePsnr> decodeKeyframePsnr(const QByteArray& packet,
                                            const H26xParameterSets& parameterSets,
                                            const AVFrame* source, QString* error) {
    if (!source) return std::nullopt;
    const CompressedAccessUnit unit = makeH264Unit(packet, parameterSets);
    if (unit.annexB.isEmpty()) {
        if (error) *error = QStringLiteral("packet did not contain length-prefixed H.264 NALs");
        return std::nullopt;
    }

    NativeVideoDecoder decoder(source->width, source->height);
    bool gotFrame = false;
    PlanePsnr psnr;
    QString decErr;
    const bool ok = decoder.decode(
        unit,
        [&](AVFrame* f) {
            if (!gotFrame && f->format == AV_PIX_FMT_YUV420P && f->width == source->width &&
                f->height == source->height) {
                psnr.y = psnrY8(source->data[0], source->linesize[0], f->data[0], f->linesize[0],
                                f->width, f->height);
                psnr.u = psnrY8(source->data[1], source->linesize[1], f->data[1], f->linesize[1],
                                (f->width + 1) / 2, (f->height + 1) / 2);
                psnr.v = psnrY8(source->data[2], source->linesize[2], f->data[2], f->linesize[2],
                                (f->width + 1) / 2, (f->height + 1) / 2);
                gotFrame = true;
            }
            av_frame_free(&f);
        },
        &decErr);
    if (!ok && !decErr.isEmpty()) {
        if (error) *error = decErr;
        return std::nullopt;
    }
    if (!gotFrame && error) *error = QStringLiteral("decoder produced no matching YUV420P frame");
    return gotFrame ? std::optional<PlanePsnr>(psnr) : std::nullopt;
}
#endif

class SpsBitReader {
public:
    SpsBitReader(const quint8* data, int size, int startByte)
        : m_data(data), m_size(size), m_byte(startByte) {}

    int bit() {
        if (m_byte >= m_size) {
            m_overrun = true;
            return 0;
        }
        if (m_zeroes >= 2 && m_data[m_byte] == 0x03 && m_bit == 0) {
            ++m_byte;
            m_zeroes = 0;
            if (m_byte >= m_size) {
                m_overrun = true;
                return 0;
            }
        }
        const int value = (m_data[m_byte] >> (7 - m_bit)) & 1;
        if (m_bit == 0) {
            if (m_data[m_byte] == 0x00)
                ++m_zeroes;
            else
                m_zeroes = 0;
        }
        if (++m_bit == 8) {
            m_bit = 0;
            ++m_byte;
        }
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

    bool overrun() const { return m_overrun; }

private:
    const quint8* m_data = nullptr;
    int m_size = 0;
    int m_byte = 0;
    int m_bit = 0;
    int m_zeroes = 0;
    bool m_overrun = false;
};

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

bool spsColourDescription(const QByteArray& nal, VuiColorCodePoints* out) {
    if (!out || nal.size() < 4) return false;
    const auto* data = reinterpret_cast<const quint8*>(nal.constData());
    const int nalSize = int(nal.size());
    if ((data[0] & 0x1f) != 7) return false;

    SpsBitReader r(data, nalSize, 1);
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
        if (r.bit()) return false;
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
    const bool fullRange = r.bit() != 0;
    if (!r.bit()) return false;
    out->colourPrimaries = int(r.bits(8));
    out->transferCharacteristics = int(r.bits(8));
    out->matrixCoefficients = int(r.bits(8));
    out->fullRange = fullRange;
    return !r.overrun();
}

} // namespace

class TestH264RoundTrip : public QObject {
    Q_OBJECT
private slots:
    void encodeMuxDemuxYieldsIntraH264();
    void encodedSpsCarriesRequestedColorVui();
    void keepSurfaceCallbackRejectionAllowsCpuFallback();
    void decodeGpuEncodeGpuRoundTripMatchesCpuUpload();

private:
    QTemporaryDir m_home;
};

void TestH264RoundTrip::encodedSpsCarriesRequestedColorVui() {
#ifdef _WIN32
    if (!qEnvironmentVariableIsSet("OLR_RUN_UNSTABLE_MF_H264_TESTS")) {
        QSKIP("Windows Media Foundation H.264 round-trip is opt-in on this machine");
    }
#endif

    QString err;
    NativeVideoEncoder::Config cfg{320, 240, 30, 1, 4'000'000};
    cfg.color.matrix = ColorMatrix::Bt601;
    cfg.color.primaries = ColorPrimaries::Bt601;
    cfg.color.transfer = ColorTransfer::Bt601;
    cfg.color.range = ColorRange::Full;
    auto enc = NativeVideoEncoder::create(cfg, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    AVFrame* f = av_frame_alloc();
    auto freeF = qScopeGuard([&] { av_frame_free(&f); });
    f->format = AV_PIX_FMT_YUV420P;
    f->width = 320;
    f->height = 240;
    QVERIFY(av_frame_get_buffer(f, 32) >= 0);
    memset(f->data[0], 128, f->linesize[0] * 240);
    memset(f->data[1], 128, f->linesize[1] * 120);
    memset(f->data[2], 128, f->linesize[2] * 120);

    bool got = false;
    auto capturePacket = [&](const QByteArray& data, int64_t, bool) {
        if (!data.isEmpty()) got = true;
    };
    QVERIFY(enc->encode(f, 0, NativeVideoEncoder::PacketCallback::bind(capturePacket), &err));
    if (!got) QSKIP("encoder produced no priming packet");
    const QByteArray avcc = enc->avccExtradata();
    if (avcc.isEmpty()) QSKIP("encoder exposed no avcC");

    QList<QByteArray> sps;
    QList<QByteArray> pps;
    QVERIFY(parseAvcc(avcc, &sps, &pps));
    QVERIFY(!sps.isEmpty());
    VuiColorCodePoints got601;
    QVERIFY2(spsColourDescription(sps.first(), &got601),
             "encoded SPS carries no colour_description_present VUI");
    QCOMPARE(got601.colourPrimaries, 6);
    QCOMPARE(got601.transferCharacteristics, 6);
    QCOMPARE(got601.matrixCoefficients, 6);
    QCOMPARE(got601.fullRange, true);
}

void TestH264RoundTrip::encodeMuxDemuxYieldsIntraH264() {
#ifdef _WIN32
    if (!qEnvironmentVariableIsSet("OLR_RUN_UNSTABLE_MF_H264_TESTS")) {
        QSKIP("Windows Media Foundation H.264 round-trip is opt-in on this machine");
    }
#endif

    QString err;
    auto enc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    AVFrame* source = makePattern();
    QVERIFY(source != nullptr);
    auto freeSource = qScopeGuard([&] { av_frame_free(&source); });

    // Prime to obtain avcC.
    bool gotPrimePacket = false;
    bool primeKeyframe = true;
    auto capturePrimePacket = [&](const QByteArray& data, int64_t, bool key) {
        if (!data.isEmpty()) {
            gotPrimePacket = true;
            primeKeyframe = key;
        }
    };
    const bool primeOk =
        enc->encode(source, 0, NativeVideoEncoder::PacketCallback::bind(capturePrimePacket), &err);
    if (!primeOk || !gotPrimePacket) {
        QSKIP("hardware H.264 encoder opened but produced no priming packet");
    }
    if (!primeKeyframe) {
        QSKIP("hardware H.264 encoder opened but does not honor all-intra keyframe output");
    }
    const QByteArray avcc = enc->avccExtradata();
    if (avcc.isEmpty()) {
        QSKIP("hardware H.264 encoder opened but did not expose avcC after priming encode");
    }

    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_h264_rt"), 1, 640, 480, 30, names, 48000, 2,
                   VideoCodecChoice::H264Hardware, avcc));

    AVStream* st = m.getStream(0);
    QVERIFY(st);
    int written = 0;
    auto writeEncodedPacket = [&](const QByteArray& data, int64_t pts, bool key) {
        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, data.size());
        memcpy(pkt->data, data.constData(), data.size());
        pkt->stream_index = 0;
        pkt->pts = pkt->dts = av_rescale_q(pts, AVRational{1, 30}, st->time_base);
        pkt->duration = av_rescale_q(1, AVRational{1, 30}, st->time_base);
        if (key) pkt->flags |= AV_PKT_FLAG_KEY;
        m.writePacket(pkt);
        av_packet_free(&pkt);
        ++written;
    };
    for (int i = 1; i <= 6; ++i) {
        enc->encode(source, i, NativeVideoEncoder::PacketCallback::bind(writeEncodedPacket), &err);
    }
    m.close();
    QVERIFY(written >= 6);

    const QString path = m_home.path() + "/olr_h264_rt.mkv";
    AVFormatContext* ctx = nullptr;
    QVERIFY(avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) >= 0);
    auto closeInput = qScopeGuard([&] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    int videoIdx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIdx = int(i);
            break;
        }
    QVERIFY(videoIdx >= 0);
    QCOMPARE(ctx->streams[videoIdx]->codecpar->codec_id, AV_CODEC_ID_H264);
    QVERIFY(ctx->streams[videoIdx]->codecpar->extradata_size > 0);

    int frames = 0, keyframes = 0;
    AVPacket* pkt = av_packet_alloc();
    auto freePkt = qScopeGuard([&] { av_packet_free(&pkt); });
    while (av_read_frame(ctx, pkt) >= 0) {
        if (pkt->stream_index == videoIdx) {
            ++frames;
            if (pkt->flags & AV_PKT_FLAG_KEY) ++keyframes;
        }
        av_packet_unref(pkt);
    }
    QCOMPARE(frames, keyframes); // all-intra
    QVERIFY(frames >= 6);

    // --- Task 7: decode-back pass via NativeVideoDecoder ---
    if (!queryNativeVideoDecodeCapabilities().h264)
        QSKIP("no hardware H.264 decoder on this platform");

    // Parse avcC extradata into SPS/PPS NAL payloads (raw, no start codes).
    // avcC layout: [0]=0x01 [1..3]=profile/compat/level [4]=0xFF [5]=0xE0|numSPS
    //   then numSPS * (2-byte big-endian length + that many bytes)
    //   then 1 byte numPPS
    //   then numPPS * (2-byte big-endian length + that many bytes)
    const uint8_t* extradata = ctx->streams[videoIdx]->codecpar->extradata;
    const int extradataSize = ctx->streams[videoIdx]->codecpar->extradata_size;
    QVERIFY(extradataSize >= 8); // minimum viable avcC

    H26xParameterSets parameterSets;
    int offset = 5; // skip configurationVersion, profile, compat, level, lengthSizeMinusOne
    const int numSps = extradata[offset] & 0x1f;
    offset++;
    for (int i = 0; i < numSps && offset + 2 <= extradataSize; ++i) {
        const int len = (extradata[offset] << 8) | extradata[offset + 1];
        offset += 2;
        QVERIFY(offset + len <= extradataSize);
        parameterSets.h264Sps.append(
            QByteArray(reinterpret_cast<const char*>(extradata + offset), len));
        offset += len;
    }
    QVERIFY(offset + 1 <= extradataSize);
    const int numPps = extradata[offset];
    offset++;
    for (int i = 0; i < numPps && offset + 2 <= extradataSize; ++i) {
        const int len = (extradata[offset] << 8) | extradata[offset + 1];
        offset += 2;
        QVERIFY(offset + len <= extradataSize);
        parameterSets.h264Pps.append(
            QByteArray(reinterpret_cast<const char*>(extradata + offset), len));
        offset += len;
    }
    QVERIFY(!parameterSets.h264Sps.isEmpty());
    QVERIFY(!parameterSets.h264Pps.isEmpty());

    // Re-open the file and feed the first video packet through NativeVideoDecoder.
    // MKV stores H.264 as avcC length-prefixed NALUs (4-byte BE length + payload).
    // Convert to Annex B (\x00\x00\x00\x01 + NAL) for the decoder's annexB field.
    AVFormatContext* decCtx = nullptr;
    QVERIFY(avformat_open_input(&decCtx, path.toUtf8().constData(), nullptr, nullptr) >= 0);
    auto closeDecInput = qScopeGuard([&] { avformat_close_input(&decCtx); });
    QVERIFY(avformat_find_stream_info(decCtx, nullptr) >= 0);

    NativeVideoDecoder decoder(640, 480);
    bool gotFrame = false;
    int frameWidth = 0, frameHeight = 0;
    double lumaPsnr = 0.0;
    double chromaUPsnr = 0.0;
    double chromaVPsnr = 0.0;

    AVPacket* decPkt = av_packet_alloc();
    auto freeDecPkt = qScopeGuard([&] { av_packet_free(&decPkt); });

    while (av_read_frame(decCtx, decPkt) >= 0 && !gotFrame) {
        if (decPkt->stream_index != videoIdx) {
            av_packet_unref(decPkt);
            continue;
        }

        // Convert avcC length-prefixed → Annex B.
        QByteArray annexB;
        const uint8_t* p = decPkt->data;
        const uint8_t* end = p + decPkt->size;
        static const char kStartCode[4] = {'\x00', '\x00', '\x00', '\x01'};
        while (p + 4 <= end) {
            const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                    (uint32_t(p[2]) << 8) | uint32_t(p[3]);
            p += 4;
            if (nalLen == 0 || p + nalLen > end) break;
            annexB.append(kStartCode, 4);
            annexB.append(reinterpret_cast<const char*>(p), int(nalLen));
            p += nalLen;
        }
        av_packet_unref(decPkt);

        if (annexB.isEmpty()) continue;

        CompressedAccessUnit unit;
        unit.codec = NativeVideoCodec::H264;
        unit.parameterSets = parameterSets;
        unit.pts90k = 0;
        unit.dts90k = 0;
        unit.annexB = annexB;

        QString decErr;
        bool ok = decoder.decode(
            unit,
            [&](AVFrame* f) {
                gotFrame = true;
                frameWidth = f->width;
                frameHeight = f->height;
                // Objective quality: PSNR of the decoded keyframe vs the source pattern, per plane.
                // Both are YUV420P with (possibly padded) per-row linesize, so compare by stride.
                // Chroma planes are half-resolution; the two distinct chroma gradients make a U<->V
                // swap or dropped chroma show up as a low chroma PSNR even though luma is
                // untouched.
                if (f->format == AV_PIX_FMT_YUV420P && f->width == source->width &&
                    f->height == source->height) {
                    lumaPsnr = psnrY8(source->data[0], source->linesize[0], f->data[0],
                                      f->linesize[0], f->width, f->height);
                    chromaUPsnr = psnrY8(source->data[1], source->linesize[1], f->data[1],
                                         f->linesize[1], f->width / 2, f->height / 2);
                    chromaVPsnr = psnrY8(source->data[2], source->linesize[2], f->data[2],
                                         f->linesize[2], f->width / 2, f->height / 2);
                }
                av_frame_free(&f);
            },
            &decErr);
        if (!ok && !decErr.isEmpty()) qWarning() << "NativeVideoDecoder error:" << decErr;
    }

    QVERIFY2(gotFrame, "NativeVideoDecoder produced no frames from the muxed H.264");
    QCOMPARE(frameWidth, 640);
    QCOMPARE(frameHeight, 480);

    // Objective fidelity gate (T3.4): the decoded frame must match the source within per-plane PSNR
    // floors. The high-frequency dither makes the 4 Mbps all-intra budget actually bind (it pulls
    // the achievable luma PSNR down from ~50 dB on the flat content to ~48 dB), so the luma floor
    // is a real quality threshold — a gross bitrate/quality regression or any corruption drops
    // below it, not only a fully garbled decode; the chroma floors catch a U<->V swap or dropped
    // chroma that luma-only would miss. Floors are calibrated on VideoToolbox (measured luma ~47.8
    // dB, chroma ~52 dB, deterministic) and sit a wide margin below that to tolerate Media
    // Foundation / other HW-encoder variance while still failing a
    // garbled/wrong-pixels/wrong-chroma decode.
    constexpr double kMinLumaPsnrDb = 40.0;
    constexpr double kMinChromaPsnrDb = 42.0;
    qInfo("decoded PSNR: luma=%.2f dB (floor %.1f), U=%.2f dB, V=%.2f dB (floor %.1f)", lumaPsnr,
          kMinLumaPsnrDb, chromaUPsnr, chromaVPsnr, kMinChromaPsnrDb);
    QVERIFY2(lumaPsnr >= kMinLumaPsnrDb,
             qPrintable(QStringLiteral("decoded luma PSNR %1 dB below floor %2 dB")
                            .arg(lumaPsnr, 0, 'f', 2)
                            .arg(kMinLumaPsnrDb, 0, 'f', 1)));
    QVERIFY2(chromaUPsnr >= kMinChromaPsnrDb && chromaVPsnr >= kMinChromaPsnrDb,
             qPrintable(QStringLiteral("decoded chroma PSNR U=%1 V=%2 dB below floor %3 dB")
                            .arg(chromaUPsnr, 0, 'f', 2)
                            .arg(chromaVPsnr, 0, 'f', 2)
                            .arg(kMinChromaPsnrDb, 0, 'f', 1)));
}

void TestH264RoundTrip::keepSurfaceCallbackRejectionAllowsCpuFallback() {
#if !defined(__APPLE__) || !defined(OLR_GPU_PIPELINE_BUILD)
    QSKIP("keep-surface rejection is exercised on the VideoToolbox GPU pipeline path");
#else
    if (!queryNativeVideoDecodeCapabilities().h264) QSKIP("no hardware H.264 decoder");

    QString err;
    auto enc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder");

    AVFrame* source = makePattern();
    QVERIFY(source != nullptr);
    auto freeSource = qScopeGuard([&] { av_frame_free(&source); });

    QByteArray refPacket;
    auto captureRefPacket = [&](const QByteArray& data, int64_t, bool key) {
        if (!data.isEmpty() && key && refPacket.isEmpty()) refPacket = data;
    };
    QVERIFY2(
        enc->encode(source, 0, NativeVideoEncoder::PacketCallback::bind(captureRefPacket), &err),
        qPrintable(err));
    if (refPacket.isEmpty()) QSKIP("encoder produced no keyframe");

    H26xParameterSets refSets;
    QVERIFY(parameterSetsFromAvcc(enc->avccExtradata(), &refSets));
    const CompressedAccessUnit unit = makeH264Unit(refPacket, refSets);
    QVERIFY(!unit.annexB.isEmpty());

    NativeVideoDecoder keepSurfaceDecoder(640, 480);
    bool callbackReached = false;
    const bool keptSurface = keepSurfaceDecoder.decodeKeepSurface(
        unit,
        [&](void*, qint64) {
            callbackReached = true;
            return false;
        },
        &err);
    QVERIFY(callbackReached);
    QVERIFY2(!keptSurface, "a rejected keep-surface callback must report decode failure");

    keepSurfaceDecoder.reset();
    bool gotCpuFrame = false;
    QVERIFY2(keepSurfaceDecoder.decode(
                 unit,
                 [&](AVFrame* frame) {
                     gotCpuFrame = frame != nullptr;
                     av_frame_free(&frame);
                 },
                 &err),
             qPrintable(err));
    QVERIFY(gotCpuFrame);
#endif
}

void TestH264RoundTrip::decodeGpuEncodeGpuRoundTripMatchesCpuUpload() {
#if !defined(__APPLE__) || !defined(OLR_GPU_PIPELINE_BUILD)
    QSKIP("GPU round-trip capstone runs on the VideoToolbox GPU pipeline path");
#else
    if (!queryNativeVideoDecodeCapabilities().h264) QSKIP("no hardware H.264 decoder");

    QString err;
    auto enc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder");

    AVFrame* source = makePattern();
    QVERIFY(source != nullptr);
    auto freeSource = qScopeGuard([&] { av_frame_free(&source); });

    QByteArray refPacket;
    bool gotKeyframe = false;
    auto captureRefPacket = [&](const QByteArray& data, int64_t, bool key) {
        if (!data.isEmpty() && key && refPacket.isEmpty()) {
            refPacket = data;
            gotKeyframe = true;
        }
    };
    QVERIFY2(
        enc->encode(source, 0, NativeVideoEncoder::PacketCallback::bind(captureRefPacket), &err),
        qPrintable(err));
    if (!gotKeyframe) QSKIP("encoder produced no keyframe");

    H26xParameterSets refSets;
    QVERIFY(parameterSetsFromAvcc(enc->avccExtradata(), &refSets));

    auto rhi = GpuRhiContext::create();
    if (!rhi) rhi = GpuRhiContext::createNullForTest();
    if (!rhi) QSKIP("no RHI context available for GPU frame ownership");

    NativeVideoDecoder keepSurfaceDecoder(640, 480);
    FrameHandle gpuFrame;
    const CompressedAccessUnit keepSurfaceUnit = makeH264Unit(refPacket, refSets);
    QVERIFY(!keepSurfaceUnit.annexB.isEmpty());
    const bool keptSurface = keepSurfaceDecoder.decodeKeepSurface(
        keepSurfaceUnit,
        [&](void* cvImageBuffer, qint64) {
            FrameMetadata meta;
            meta.key.format = FramePixelFormat::Nv12;
            meta.key.width = 640;
            meta.key.height = 480;
            gpuFrame = importVtImageBuffer(cvImageBuffer, meta, rhi);
            return !gpuFrame.isNull();
        },
        &err);
    if (!keptSurface || gpuFrame.isNull() || !gpuFrame.isGpuBacked()) {
        QSKIP("keep-surface decode did not yield a GPU-backed IOSurface handle");
    }

    auto gpuEnc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    QVERIFY2(gpuEnc != nullptr, qPrintable(err));

    QByteArray gpuPacket;
    auto captureGpuPacket = [&](const QByteArray& data, int64_t, bool key) {
        if (!data.isEmpty() && key && gpuPacket.isEmpty()) gpuPacket = data;
    };
    QVERIFY2(gpuEnc->encodeSurface(gpuFrame.data()->gpuSurface(), 0, gpuFrame.metadata().color,
                                   NativeVideoEncoder::PacketCallback::bind(captureGpuPacket),
                                   &err),
             qPrintable(err));
    QVERIFY(!gpuPacket.isEmpty());

    H26xParameterSets gpuSets;
    QVERIFY(parameterSetsFromAvcc(gpuEnc->avccExtradata(), &gpuSets));

    QString cpuDecodeErr;
    const std::optional<PlanePsnr> cpuPsnr =
        decodeKeyframePsnr(refPacket, refSets, source, &cpuDecodeErr);
    QVERIFY2(cpuPsnr.has_value(), qPrintable(cpuDecodeErr));

    QString gpuDecodeErr;
    const std::optional<PlanePsnr> gpuPsnr =
        decodeKeyframePsnr(gpuPacket, gpuSets, source, &gpuDecodeErr);
    QVERIFY2(gpuPsnr.has_value(), qPrintable(gpuDecodeErr));

    qInfo("round-trip PSNR: cpu-upload Y=%.2f U=%.2f V=%.2f dB; gpu-direct Y=%.2f U=%.2f "
          "V=%.2f dB",
          cpuPsnr->y, cpuPsnr->u, cpuPsnr->v, gpuPsnr->y, gpuPsnr->u, gpuPsnr->v);

    constexpr double kMinLumaPsnrDb = 40.0;
    constexpr double kMinChromaPsnrDb = 38.0;
    QVERIFY2(gpuPsnr->y >= kMinLumaPsnrDb,
             qPrintable(QStringLiteral("GPU-direct luma PSNR %1 dB below floor %2 dB")
                            .arg(gpuPsnr->y, 0, 'f', 2)
                            .arg(kMinLumaPsnrDb, 0, 'f', 1)));
    QVERIFY2(gpuPsnr->u >= kMinChromaPsnrDb && gpuPsnr->v >= kMinChromaPsnrDb,
             qPrintable(QStringLiteral("GPU-direct chroma PSNR U=%1 V=%2 dB below floor %3 dB")
                            .arg(gpuPsnr->u, 0, 'f', 2)
                            .arg(gpuPsnr->v, 0, 'f', 2)
                            .arg(kMinChromaPsnrDb, 0, 'f', 1)));
    QVERIFY2(std::abs(gpuPsnr->y - cpuPsnr->y) <= 3.0,
             qPrintable(QStringLiteral("GPU-direct luma PSNR %1 dB diverges from CPU-upload %2 dB")
                            .arg(gpuPsnr->y, 0, 'f', 2)
                            .arg(cpuPsnr->y, 0, 'f', 2)));
    QVERIFY2(std::abs(gpuPsnr->u - cpuPsnr->u) <= 4.0 && std::abs(gpuPsnr->v - cpuPsnr->v) <= 4.0,
             qPrintable(QStringLiteral("GPU-direct chroma PSNR U=%1 V=%2 diverges from "
                                       "CPU-upload U=%3 V=%4 dB")
                            .arg(gpuPsnr->u, 0, 'f', 2)
                            .arg(gpuPsnr->v, 0, 'f', 2)
                            .arg(cpuPsnr->u, 0, 'f', 2)
                            .arg(cpuPsnr->v, 0, 'f', 2)));
#endif
}

QTEST_GUILESS_MAIN(TestH264RoundTrip)
#include "tst_h264_roundtrip.moc"
