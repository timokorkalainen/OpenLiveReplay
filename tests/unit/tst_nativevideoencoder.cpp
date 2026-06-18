// Unit tests for NativeVideoEncoder. Behavioral encode tests are gated on the
// platform actually having a hardware H.264 encoder; where absent, create()
// must return nullptr gracefully (never a software fallback).
#include <QtTest>

#include "recorder_engine/codec/nativevideoencoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

class TestNativeVideoEncoder : public QObject {
    Q_OBJECT
private slots:
    void capabilityProbeIsConsistentWithCreate();
    void encodesIntraFramesWhenAvailable();

private:
    static AVFrame* makeGreyFrame(int w, int h);
};

AVFrame* TestNativeVideoEncoder::makeGreyFrame(int w, int h) {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    av_frame_get_buffer(f, 32);
    memset(f->data[0], 128, f->linesize[0] * h);
    memset(f->data[1], 128, f->linesize[1] * (h / 2));
    memset(f->data[2], 128, f->linesize[2] * (h / 2));
    return f;
}

void TestNativeVideoEncoder::capabilityProbeIsConsistentWithCreate() {
    const NativeVideoEncodeCapabilities caps = queryNativeVideoEncodeCapabilities();
    QString err;
    auto enc = NativeVideoEncoder::create({1280, 720, 30, 1, 8'000'000}, &err);
    if (caps.h264) {
        QVERIFY2(enc != nullptr, qPrintable("caps say h264 but create failed: " + err));
    } else {
        QVERIFY2(enc == nullptr, "caps say no h264 but create returned an encoder");
    }
}

void TestNativeVideoEncoder::encodesIntraFramesWhenAvailable() {
    QString err;
    auto enc = NativeVideoEncoder::create({1280, 720, 30, 1, 8'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    int packets = 0;
    bool allKeyframes = true;
    for (int i = 0; i < 5; ++i) {
        AVFrame* f = makeGreyFrame(1280, 720);
        const bool ok = enc->encode(f, i, [&](const QByteArray& data, int64_t, bool key) {
            ++packets;
            if (!key) allKeyframes = false;
            QVERIFY(!data.isEmpty());
        }, &err);
        av_frame_free(&f);
        QVERIFY2(ok, qPrintable(err));
    }
    enc->flush([&](const QByteArray&, int64_t, bool key) {
        ++packets;
        if (!key) allKeyframes = false;
    }, &err);

    QVERIFY2(packets >= 5, "expected at least one packet per submitted frame");
    QVERIFY2(allKeyframes, "all-intra: every packet must be a keyframe");
    QVERIFY2(!enc->avccExtradata().isEmpty(), "avcC must be available after encoding");
    QCOMPARE(quint8(enc->avccExtradata().at(0)), quint8(0x01)); // configurationVersion
}

QTEST_GUILESS_MAIN(TestNativeVideoEncoder)
#include "tst_nativevideoencoder.moc"
