// Phase-0 probe P0.1: prove the VideoToolbox decode session hands back an
// IOSurface-backed CVPixelBuffer (precondition for zero-copy CVMetalTextureCache
// import in gpu-abstraction). Apple-only behavioral test; QSKIPs elsewhere.
#include <QtTest>

#include "recorder_engine/codec/avcc.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/ingest/h26xaccessunit.h"
#include "recorder_engine/ingest/nativevideodecoder.h"

#include <QElapsedTimer>

#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
}

class TestVtIOSurface : public QObject {
    Q_OBJECT
private slots:
    void decodedBufferIsIOSurfaceBacked();
    void reconfigCostIsBounded();

private:
    static AVFrame* makeGreyFrame(int w, int h);
    static QByteArray avccPacketToAnnexB(const QByteArray& packet);
    static bool buildGreyIdrAccessUnit(CompressedAccessUnit* unit, int w, int h);
};

AVFrame* TestVtIOSurface::makeGreyFrame(int w, int h) {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    memset(f->data[0], 128, f->linesize[0] * h);
    memset(f->data[1], 128, f->linesize[1] * (h / 2));
    memset(f->data[2], 128, f->linesize[2] * (h / 2));
    return f;
}

QByteArray TestVtIOSurface::avccPacketToAnnexB(const QByteArray& packet) {
    QByteArray annexB;
    const auto* p = reinterpret_cast<const uint8_t*>(packet.constData());
    const auto* end = p + packet.size();
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
    return annexB;
}

bool TestVtIOSurface::buildGreyIdrAccessUnit(CompressedAccessUnit* unit, int w, int h) {
    if (!unit) return false;

    QString err;
    auto enc = NativeVideoEncoder::create({w, h, 30, 1, 4'000'000}, &err);
    if (!enc) return false;

    QByteArray packet;
    bool keyframe = false;
    AVFrame* frame = makeGreyFrame(w, h);
    if (!frame) return false;
    const bool ok = enc->encode(
        frame, 0,
        [&](const QByteArray& data, int64_t, bool key) {
            if (!data.isEmpty()) {
                packet = data;
                keyframe = key;
            }
        },
        &err);
    av_frame_free(&frame);
    if (!ok || packet.isEmpty() || !keyframe) return false;

    QList<QByteArray> sps;
    QList<QByteArray> pps;
    if (!parseAvcc(enc->avccExtradata(), &sps, &pps)) return false;

    unit->codec = NativeVideoCodec::H264;
    unit->pts90k = 0;
    unit->dts90k = 0;
    unit->parameterSets.h264Sps = sps;
    unit->parameterSets.h264Pps = pps;
    unit->annexB = avccPacketToAnnexB(packet);
    return !unit->annexB.isEmpty();
}

void TestVtIOSurface::decodedBufferIsIOSurfaceBacked() {
    const NativeVideoDecodeCapabilities caps = queryNativeVideoDecodeCapabilities();
    if (!caps.h264) QSKIP("no VideoToolbox H.264 decode on this platform");

    CompressedAccessUnit unit;
    if (!buildGreyIdrAccessUnit(&unit, 1280, 720))
        QSKIP("could not author a test IDR access unit on this platform");

    NativeVideoDecoder decoder(0, 0);
    int frames = 0;
    QString err;
    const bool ok = decoder.decode(
        unit,
        [&](AVFrame* f) {
            ++frames;
            av_frame_free(&f);
        },
        &err);

    QVERIFY2(ok, qPrintable(err));
    QCOMPARE(frames, 1);
    QVERIFY2(decoder.lastDecodedWasIOSurfaceBacked(),
             "VT decode session did not produce an IOSurface-backed CVPixelBuffer");
}

void TestVtIOSurface::reconfigCostIsBounded() {
    const NativeVideoDecodeCapabilities caps = queryNativeVideoDecodeCapabilities();
    if (!caps.h264) QSKIP("no VideoToolbox H.264 decode on this platform");

    CompressedAccessUnit a;
    CompressedAccessUnit b;
    if (!buildGreyIdrAccessUnit(&a, 1280, 720) || !buildGreyIdrAccessUnit(&b, 640, 480))
        QSKIP("could not author two distinct-geometry IDR access units");

    NativeVideoDecoder decoder(0, 0);
    QString err;
    auto drain = [](AVFrame* f) { av_frame_free(&f); };
    QVERIFY2(decoder.decode(a, drain, &err), qPrintable(err));

    QList<qint64> samples;
    for (int i = 0; i < 8; ++i) {
        const CompressedAccessUnit& unit = (i % 2 == 0) ? b : a;
        QElapsedTimer t;
        t.start();
        QVERIFY2(decoder.decode(unit, drain, &err), qPrintable(err));
        samples.append(t.nsecsElapsed());
    }

    std::sort(samples.begin(), samples.end());
    const double medianMs = samples.at(samples.size() / 2) / 1.0e6;
    qInfo("P0.1 reconfig median (decode incl. session recreate): %.3f ms", medianMs);
    QVERIFY2(medianMs < 100.0, "VT reconfig wildly over budget (>100 ms)");
}

QTEST_GUILESS_MAIN(TestVtIOSurface)
#include "tst_vtiosurface.moc"
