#include <QtTest>

#include "recorder_engine/ingest/nativevideodecoder.h"

class TestNativeVideoDecoder : public QObject {
    Q_OBJECT
private slots:
    void defaultCapabilitiesAreFalse();
    void queryCapabilitiesReportsPlatformBackend();
    void keepSurfaceNullImageBufferIsRejected();
    void videoToolboxNoFrameIsRejected();
    void flushExcessPixelBufferPoolNoOpsWithoutSession();
    void mediaFoundationPrefixesParameterSetsAtSessionStart();
    void decodedOutputPtsCorrectsBufferedFrameTiming();
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

void TestNativeVideoDecoder::videoToolboxNoFrameIsRejected() {
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_TVOS) || defined(Q_OS_WATCHOS)
    QString error;
    QVERIFY(!nativeVideoDecoderNoFrameRejectedForTest(&error));
    QVERIFY(error.contains(QStringLiteral("produced no frame")));
#else
    QSKIP("VideoToolbox no-frame validation seam is Apple-only");
#endif
}

void TestNativeVideoDecoder::flushExcessPixelBufferPoolNoOpsWithoutSession() {
    NativeVideoDecoder decoder(0, 0);
    decoder.flushExcessPixelBufferPool();
}

void TestNativeVideoDecoder::mediaFoundationPrefixesParameterSetsAtSessionStart() {
#if defined(Q_OS_WIN)
    CompressedAccessUnit unit;
    unit.codec = NativeVideoCodec::Hevc;
    unit.parameterSets.hevcVps = {QByteArray::fromHex("4001")};
    unit.parameterSets.hevcSps = {QByteArray::fromHex("4201")};
    unit.parameterSets.hevcPps = {QByteArray::fromHex("4401")};
    unit.annexB = QByteArray::fromHex("000000012601");

    QCOMPARE(nativeVideoDecoderInputBytesForTest(unit, true),
             QByteArray::fromHex("000000014001000000014201000000014401000000012601"));
    QCOMPARE(nativeVideoDecoderInputBytesForTest(unit, false), unit.annexB);

    unit.annexB = QByteArray::fromHex("0000000140010000014201000000014401000000012601");
    QCOMPARE(nativeVideoDecoderInputBytesForTest(unit, true), unit.annexB);
#else
    QSKIP("Media Foundation input assembly is Windows-only");
#endif
}

void TestNativeVideoDecoder::decodedOutputPtsCorrectsBufferedFrameTiming() {
    QCOMPARE(nativeVideoDecodedSourcePtsMs(216000, 2400, 180000), qint64(2000));
    QCOMPARE(nativeVideoDecodedSourcePtsMs(216000, 2400, 216000), qint64(2400));

    constexpr qint64 wrap90k = qint64(1) << 33;
    QCOMPARE(nativeVideoDecodedSourcePtsMs(4500, 1000, wrap90k - 4500), qint64(900));
    constexpr qint64 rtmpWrap90k = (qint64(1) << 32) * 90;
    QCOMPARE(nativeVideoDecodedSourcePtsMs(4500, 1000, rtmpWrap90k - 4500, rtmpWrap90k),
             qint64(900));
    QCOMPARE(nativeVideoDecodedSourcePtsMs(-1, 2400, 180000), qint64(2400));
    QCOMPARE(nativeVideoDecodedTimecode100ns(216000, 100000000, 180000), qint64(96000000));
    QCOMPARE(nativeVideoDecodedTimecode100ns(9000, 500000, 0), qint64(863999500000));
}

QTEST_GUILESS_MAIN(TestNativeVideoDecoder)
#include "tst_nativevideodecoder.moc"
