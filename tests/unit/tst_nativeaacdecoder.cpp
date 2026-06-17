#include <QtTest>

#include "recorder_engine/ingest/nativeaacdecoder.h"

namespace {

QByteArray adtsFrame(int sampleRateIndex, int channelConfig, int payloadSize,
                     bool protectionAbsent = true)
{
    const int headerSize = protectionAbsent ? 7 : 9;
    const int frameLength = headerSize + payloadSize;
    QByteArray frame(headerSize + payloadSize, char(0x5a));
    frame[0] = char(0xff);
    frame[1] = char(0xf0 | (protectionAbsent ? 0x01 : 0x00));
    frame[2] = char((1 << 6) | ((sampleRateIndex & 0x0f) << 2)
                    | ((channelConfig >> 2) & 0x01));
    frame[3] = char(((channelConfig & 0x03) << 6) | ((frameLength >> 11) & 0x03));
    frame[4] = char((frameLength >> 3) & 0xff);
    frame[5] = char(((frameLength & 0x07) << 5) | 0x1f);
    frame[6] = char(0xfc);
    if (!protectionAbsent) {
        frame[7] = char(0x12);
        frame[8] = char(0x34);
    }
    return frame;
}

} // namespace

class TestNativeAacDecoder : public QObject {
    Q_OBJECT

private slots:
    void parsesAdtsHeader();
    void parsesAdtsHeaderWithCrc();
    void rejectsInvalidOrTruncatedHeaders();
    void detectsLatmLoasSync();
#ifdef _WIN32
    void decodesAdtsToneWithNativeBackend();
#endif
};

void TestNativeAacDecoder::parsesAdtsHeader()
{
    const QByteArray frame = adtsFrame(3, 2, 12);
    AacAdtsFrameInfo info;

    QVERIFY(NativeAacDecoder::parseAdtsFrame(frame, 0, &info));
    QCOMPARE(info.headerSize, 7);
    QCOMPARE(info.frameSize, 19);
    QCOMPARE(info.sampleRate, 48000);
    QCOMPARE(info.channelCount, 2);
    QCOMPARE(info.samplesPerFrame, 1024);
    QCOMPARE(info.audioObjectType, 2);
}

void TestNativeAacDecoder::parsesAdtsHeaderWithCrc()
{
    const QByteArray frame = adtsFrame(4, 1, 5, false);
    AacAdtsFrameInfo info;

    QVERIFY(NativeAacDecoder::parseAdtsFrame(frame, 0, &info));
    QCOMPARE(info.headerSize, 9);
    QCOMPARE(info.frameSize, 14);
    QCOMPARE(info.sampleRate, 44100);
    QCOMPARE(info.channelCount, 1);
}

void TestNativeAacDecoder::rejectsInvalidOrTruncatedHeaders()
{
    AacAdtsFrameInfo info;

    QVERIFY(!NativeAacDecoder::parseAdtsFrame(QByteArray::fromHex("fff15080"), 0, &info));
    QVERIFY(!NativeAacDecoder::parseAdtsFrame(QByteArray::fromHex("56e000"), 0, &info));
    QVERIFY(!NativeAacDecoder::parseAdtsFrame(adtsFrame(15, 2, 12), 0, &info));
    QVERIFY(NativeAacDecoder::hasAdtsSync(QByteArray::fromHex("fff1"), 0));
    QVERIFY(!NativeAacDecoder::hasAdtsSync(QByteArray::fromHex("56e0"), 0));
}

void TestNativeAacDecoder::detectsLatmLoasSync()
{
    QVERIFY(NativeAacDecoder::hasLatmLoasSync(QByteArray::fromHex("56e000"), 0));
    QVERIFY(!NativeAacDecoder::hasLatmLoasSync(adtsFrame(3, 2, 12), 0));
}

#ifdef _WIN32
void TestNativeAacDecoder::decodesAdtsToneWithNativeBackend() {
    // 0.15 s, 48 kHz stereo AAC-LC ADTS fixture generated from a 1 kHz sine.
    const QByteArray stream = QByteArray::fromBase64(
        "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"
        "AAAAAAAAAAAA7/8UyAAd/8IUDaRgjBwA==");

    NativeAacDecoder decoder;
    QByteArray decoded;
    int offset = 0;
    while (offset < stream.size()) {
        AacAdtsFrameInfo info;
        QVERIFY2(NativeAacDecoder::parseAdtsFrame(stream, offset, &info), "embedded ADTS frame");

        QByteArray pcm;
        QString error;
        QVERIFY2(decoder.decodeAdtsFrame(stream.mid(offset, info.frameSize), info, &pcm, &error),
                 qPrintable(error));
        decoded.append(pcm);
        offset += info.frameSize;
    }

    QVERIFY2(!decoded.isEmpty(), "Media Foundation AAC decoder produced no PCM");

    int maxAbs = 0;
    const auto* samples = reinterpret_cast<const int16_t*>(decoded.constData());
    const int sampleCount = decoded.size() / int(sizeof(int16_t));
    for (int i = 0; i < sampleCount; ++i) {
        maxAbs = qMax(maxAbs, qAbs(int(samples[i])));
    }
    QVERIFY2(maxAbs > 500, "decoded AAC tone is silent");
}
#endif

QTEST_GUILESS_MAIN(TestNativeAacDecoder)
#include "tst_nativeaacdecoder.moc"
