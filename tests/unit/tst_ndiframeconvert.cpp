#include <QtTest>

#include "recorder_engine/ingest/ndiframeconvert.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

class TestNdiFrameConvert : public QObject {
    Q_OBJECT

private slots:
    void i420FastPathCopiesPlanes();
    void uyvyConvertsThroughSwscale();
    void audioFloatPlanarToS16Stereo();
    void audioMonoDuplicates();
    void audioResamplesTo48k();
};

void TestNdiFrameConvert::i420FastPathCopiesPlanes() {
    QByteArray pixels;
    pixels.append(QByteArray(16, char(50)));
    pixels.append(QByteArray(4, char(60)));
    pixels.append(QByteArray(4, char(70)));
    NdiVideoFrame in;
    in.width = 4;
    in.height = 4;
    in.strideBytes = 4;
    in.fourCc = kNdiFourCcI420;
    in.data = reinterpret_cast<const uint8_t*>(pixels.constData());

    SwsContext* cache = nullptr;
    AVFrame* frame = ndiVideoToYuv420p(in, 4, 4, &cache);
    QVERIFY(frame);
    QCOMPARE(frame->format, int(AV_PIX_FMT_YUV420P));
    QCOMPARE(frame->width, 4);
    QCOMPARE(frame->height, 4);
    QCOMPARE(uchar(frame->data[0][0]), uchar(50));
    QCOMPARE(uchar(frame->data[1][0]), uchar(60));
    QCOMPARE(uchar(frame->data[2][0]), uchar(70));
    av_frame_free(&frame);
    sws_freeContext(cache);
}

void TestNdiFrameConvert::uyvyConvertsThroughSwscale() {
    QByteArray pixels;
    for (int i = 0; i < 4 * 4 / 2; ++i) {
        pixels.append(char(128));
        pixels.append(char(100));
        pixels.append(char(128));
        pixels.append(char(100));
    }
    NdiVideoFrame in;
    in.width = 4;
    in.height = 4;
    in.strideBytes = 8;
    in.fourCc = kNdiFourCcUyvy;
    in.data = reinterpret_cast<const uint8_t*>(pixels.constData());

    SwsContext* cache = nullptr;
    AVFrame* frame = ndiVideoToYuv420p(in, 4, 4, &cache);
    QVERIFY(frame);
    QCOMPARE(frame->format, int(AV_PIX_FMT_YUV420P));
    QVERIFY(qAbs(int(uchar(frame->data[0][0])) - 100) <= 2);
    av_frame_free(&frame);
    sws_freeContext(cache);
}

void TestNdiFrameConvert::audioFloatPlanarToS16Stereo() {
    QVector<float> samples(8);
    for (int i = 0; i < 4; ++i) {
        samples[i] = 0.5f;
        samples[4 + i] = -0.5f;
    }
    NdiAudioFrame in;
    in.sampleRate = 48000;
    in.channels = 2;
    in.samples = 4;
    in.channelStrideBytes = 4 * int(sizeof(float));
    in.data = samples.constData();

    const QByteArray out = ndiAudioToS16Stereo(in);
    QCOMPARE(out.size(), 4 * 2 * int(sizeof(int16_t)));
    const auto* s16 = reinterpret_cast<const int16_t*>(out.constData());
    QCOMPARE(s16[0], int16_t(16384));
    QCOMPARE(s16[1], int16_t(-16384));
}

void TestNdiFrameConvert::audioMonoDuplicates() {
    QVector<float> samples(4, 0.25f);
    NdiAudioFrame in;
    in.sampleRate = 48000;
    in.channels = 1;
    in.samples = 4;
    in.channelStrideBytes = 4 * int(sizeof(float));
    in.data = samples.constData();

    const QByteArray out = ndiAudioToS16Stereo(in);
    const auto* s16 = reinterpret_cast<const int16_t*>(out.constData());
    QCOMPARE(s16[0], s16[1]);
}

void TestNdiFrameConvert::audioResamplesTo48k() {
    QVector<float> samples(441);
    NdiAudioFrame in;
    in.sampleRate = 44100;
    in.channels = 1;
    in.samples = samples.size();
    in.channelStrideBytes = samples.size() * int(sizeof(float));
    in.data = samples.constData();

    const QByteArray out = ndiAudioToS16Stereo(in);
    QCOMPARE(out.size(), 480 * 2 * int(sizeof(int16_t)));
}

QTEST_GUILESS_MAIN(TestNdiFrameConvert)
#include "tst_ndiframeconvert.moc"
