// End-to-end avcC + muxing round-trip: native-encode grey frames, attach the
// encoder's avcC to the muxer, write a real MKV, then demux it and assert the
// stream is H.264, every frame is a keyframe, and the frame count matches.
#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>

#include "recorder_engine/muxer.h"
#include "recorder_engine/codec/nativevideoencoder.h"

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
    enc->encode(prime, 0, [](const QByteArray&, int64_t, bool){}, &err);
    av_frame_free(&prime);
    const QByteArray avcc = enc->avccExtradata();
    QVERIFY(!avcc.isEmpty());

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
}

QTEST_GUILESS_MAIN(TestH264RoundTrip)
#include "tst_h264_roundtrip.moc"
