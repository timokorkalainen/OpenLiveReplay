#include <QtTest>

#include "recorder_engine/ingest/nativevideodecoder.h"

class TestNativeVideoDecoder : public QObject {
    Q_OBJECT
private slots:
    void defaultCapabilitiesAreFalse();
    void queryCapabilitiesReportsPlatformBackend();
    void keepSurfaceNullImageBufferIsRejected();
};

void TestNativeVideoDecoder::defaultCapabilitiesAreFalse() {
    const NativeVideoDecodeCapabilities caps;
    QVERIFY(!caps.h264);
    QVERIFY(!caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(caps.detail.isEmpty());
}

void TestNativeVideoDecoder::queryCapabilitiesReportsPlatformBackend() {
    const NativeVideoDecodeCapabilities caps = queryNativeVideoDecodeCapabilities();
#if defined(Q_OS_WIN)
    QVERIFY(!caps.detail.isEmpty());
#else
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_TVOS) || defined(Q_OS_WATCHOS)
    QVERIFY(caps.h264);
    QVERIFY(caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(caps.detail.contains(QStringLiteral("VideoToolbox")) || !caps.detail.isEmpty());
#else
    QVERIFY(!caps.h264);
    QVERIFY(!caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(!caps.detail.isEmpty());
#endif
#endif
}

void TestNativeVideoDecoder::keepSurfaceNullImageBufferIsRejected() {
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_TVOS) || defined(Q_OS_WATCHOS)
    QVERIFY(nativeVideoDecoderKeepSurfaceNullImageRejectedForTest());
#else
    QSKIP("VideoToolbox null-image callback seam is Apple-only");
#endif
}

QTEST_GUILESS_MAIN(TestNativeVideoDecoder)
#include "tst_nativevideodecoder.moc"
