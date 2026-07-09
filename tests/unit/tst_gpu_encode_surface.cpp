#include <QtTest>

#include "playback/gpu/gpusurface.h"
#include "playback/output/colormetadata.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif

class TestGpuEncodeSurface : public QObject {
    Q_OBJECT
private slots:
    void encodesSurfaceToKeyframeWithoutCpuUpload();
};

void TestGpuEncodeSurface::encodesSurfaceToKeyframeWithoutCpuUpload() {
#ifndef __APPLE__
    QSKIP("GPU-surface encode test currently exercises the VideoToolbox host path");
#else
    auto surface = makeAppleNv12Surface(320, 240);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");

    QString err;
    NativeVideoEncoder::Config cfg{320, 240, 30, 1, 4'000'000};
    auto enc = NativeVideoEncoder::create(cfg, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    bool gotKeyframe = false;
    const bool ok = enc->encodeSurface(
        surface.get(), 0, ColorMetadata{},
        [&](const QByteArray& data, int64_t, bool keyframe) {
            if (!data.isEmpty() && keyframe) gotKeyframe = true;
        },
        &err);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY(gotKeyframe);
    QVERIFY(!enc->avccExtradata().isEmpty());
#endif
}

QTEST_GUILESS_MAIN(TestGpuEncodeSurface)
#include "tst_gpu_encode_surface.moc"
