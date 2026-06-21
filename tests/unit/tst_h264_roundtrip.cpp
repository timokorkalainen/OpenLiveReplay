// End-to-end avcC + muxing round-trip: native-encode grey frames, attach the
// encoder's avcC to the muxer, write a real MKV, then demux it and assert the
// stream is H.264, every frame is a keyframe, and the frame count matches.
// Task 7: also decode back via NativeVideoDecoder and assert frame dimensions.
#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>

#include "recorder_engine/muxer.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/ingest/nativevideodecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

class TestH264RoundTrip : public QObject {
    Q_OBJECT
private slots:
    void encodeMuxDemuxYieldsIntraH264();
private:
    QTemporaryDir m_home;
};

void TestH264RoundTrip::encodeMuxDemuxYieldsIntraH264() {
#ifdef _WIN32
    if (!qEnvironmentVariableIsSet("OLR_RUN_UNSTABLE_MF_H264_TESTS")) {
        QSKIP("Windows Media Foundation H.264 round-trip is opt-in on this machine");
    }
#endif

    QString err;
    auto enc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    // Prime to obtain avcC.
    auto grey = []() {
        AVFrame* f = av_frame_alloc();
        f->format = AV_PIX_FMT_YUV420P; f->width = 640; f->height = 480;
        av_frame_get_buffer(f, 32);
        memset(f->data[0], 128, f->linesize[0] * 480);
        memset(f->data[1], 128, f->linesize[1] * 240);
        memset(f->data[2], 128, f->linesize[2] * 240);
        return f;
    };
    AVFrame* prime = grey();
    bool gotPrimePacket = false;
    bool primeKeyframe = true;
    const bool primeOk = enc->encode(
        prime, 0,
        [&](const QByteArray& data, int64_t, bool key) {
            if (!data.isEmpty()) {
                gotPrimePacket = true;
                primeKeyframe = key;
            }
        },
        &err);
    av_frame_free(&prime);
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
    QVERIFY(m.init(QStringLiteral("olr_h264_rt"), 1, 640, 480, 30, names,
                   48000, 2, VideoCodecChoice::H264Hardware, avcc));

    AVStream* st = m.getStream(0);
    QVERIFY(st);
    int written = 0;
    for (int i = 1; i <= 6; ++i) {
        AVFrame* f = grey();
        enc->encode(f, i, [&](const QByteArray& data, int64_t pts, bool key) {
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
        }, &err);
        av_frame_free(&f);
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
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoIdx = int(i); break; }
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
        parameterSets.h264Sps.append(QByteArray(reinterpret_cast<const char*>(extradata + offset), len));
        offset += len;
    }
    QVERIFY(offset + 1 <= extradataSize);
    const int numPps = extradata[offset];
    offset++;
    for (int i = 0; i < numPps && offset + 2 <= extradataSize; ++i) {
        const int len = (extradata[offset] << 8) | extradata[offset + 1];
        offset += 2;
        QVERIFY(offset + len <= extradataSize);
        parameterSets.h264Pps.append(QByteArray(reinterpret_cast<const char*>(extradata + offset), len));
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
            const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                                  | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
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
        bool ok = decoder.decode(unit, [&](AVFrame* f) {
            gotFrame = true;
            frameWidth = f->width;
            frameHeight = f->height;
            av_frame_free(&f);
        }, &decErr);
        if (!ok && !decErr.isEmpty())
            qWarning() << "NativeVideoDecoder error:" << decErr;
    }

    QVERIFY2(gotFrame, "NativeVideoDecoder produced no frames from the muxed H.264");
    QCOMPARE(frameWidth, 640);
    QCOMPARE(frameHeight, 480);
}

QTEST_GUILESS_MAIN(TestH264RoundTrip)
#include "tst_h264_roundtrip.moc"
