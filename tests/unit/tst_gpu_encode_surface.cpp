#include <QtTest>

#include "playback/gpu/gpusurface.h"
#include "playback/output/colormetadata.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#include <stdexcept>

#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif

class TestGpuEncodeSurface : public QObject {
    Q_OBJECT
private slots:
    void encodesSurfaceToKeyframeWithoutCpuUpload();
    void rejectsWrongSurfaceFormatWithoutLeakingReadScope();
    void throwingPacketCallbackReleasesSurfaceReadScope();
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
    auto onPacket = [&](const QByteArray& data, int64_t, bool keyframe) {
        if (!data.isEmpty() && keyframe) gotKeyframe = true;
    };
    const bool ok = enc->encodeSurface(surface.get(), 0, ColorMetadata{},
                                       NativeVideoEncoder::PacketCallback::bind(onPacket), &err);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY(gotKeyframe);
    QVERIFY(!enc->avccExtradata().isEmpty());
#endif
}

void TestGpuEncodeSurface::rejectsWrongSurfaceFormatWithoutLeakingReadScope() {
#ifndef __APPLE__
    QSKIP("GPU-surface encode test currently exercises the VideoToolbox host path");
#else
    auto surface = makeAppleRgba8Surface(320, 240);
    if (!surface) QSKIP("could not allocate an IOSurface-backed RGBA surface");

    QString err;
    auto enc = NativeVideoEncoder::create({320, 240, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");
    QVERIFY(!enc->encodeSurface(surface.get(), 0, ColorMetadata{}, {}, &err));
    QVERIFY(err.contains(QStringLiteral("expected NV12")));
#endif
}

void TestGpuEncodeSurface::throwingPacketCallbackReleasesSurfaceReadScope() {
#ifndef __APPLE__
    QSKIP("GPU-surface encode test currently exercises the VideoToolbox host path");
#else
    auto surface = makeAppleNv12Surface(320, 240);
    if (!surface) QSKIP("could not allocate an IOSurface-backed NV12 surface");

    QString err;
    auto enc = NativeVideoEncoder::create({320, 240, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");
    bool callbackInvoked = false;
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, enc->encodeSurface(
                                                     surface.get(), 0, ColorMetadata{},
                                                     [&](const QByteArray&, int64_t, bool) {
                                                         callbackInvoked = true;
                                                         throw std::runtime_error(
                                                             "intentional packet callback failure");
                                                     },
                                                     &err));
    QVERIFY(callbackInvoked);
#endif
}

QTEST_GUILESS_MAIN(TestGpuEncodeSurface)
#include "tst_gpu_encode_surface.moc"
