#include <QtTest>

#include "recorder_engine/ingest/audiotoolboxaacdecoder.h"

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

class TestAudioToolboxAacDecoder : public QObject {
    Q_OBJECT

private slots:
    void parsesAdtsHeader();
    void parsesAdtsHeaderWithCrc();
    void rejectsInvalidOrTruncatedHeaders();
    void detectsLatmLoasSync();
};

void TestAudioToolboxAacDecoder::parsesAdtsHeader()
{
    const QByteArray frame = adtsFrame(3, 2, 12);
    AacAdtsFrameInfo info;

    QVERIFY(AudioToolboxAacDecoder::parseAdtsFrame(frame, 0, &info));
    QCOMPARE(info.headerSize, 7);
    QCOMPARE(info.frameSize, 19);
    QCOMPARE(info.sampleRate, 48000);
    QCOMPARE(info.channelCount, 2);
    QCOMPARE(info.samplesPerFrame, 1024);
    QCOMPARE(info.audioObjectType, 2);
}

void TestAudioToolboxAacDecoder::parsesAdtsHeaderWithCrc()
{
    const QByteArray frame = adtsFrame(4, 1, 5, false);
    AacAdtsFrameInfo info;

    QVERIFY(AudioToolboxAacDecoder::parseAdtsFrame(frame, 0, &info));
    QCOMPARE(info.headerSize, 9);
    QCOMPARE(info.frameSize, 14);
    QCOMPARE(info.sampleRate, 44100);
    QCOMPARE(info.channelCount, 1);
}

void TestAudioToolboxAacDecoder::rejectsInvalidOrTruncatedHeaders()
{
    AacAdtsFrameInfo info;

    QVERIFY(!AudioToolboxAacDecoder::parseAdtsFrame(QByteArray::fromHex("fff15080"), 0, &info));
    QVERIFY(!AudioToolboxAacDecoder::parseAdtsFrame(QByteArray::fromHex("56e000"), 0, &info));
    QVERIFY(!AudioToolboxAacDecoder::parseAdtsFrame(adtsFrame(15, 2, 12), 0, &info));
    QVERIFY(AudioToolboxAacDecoder::hasAdtsSync(QByteArray::fromHex("fff1"), 0));
    QVERIFY(!AudioToolboxAacDecoder::hasAdtsSync(QByteArray::fromHex("56e0"), 0));
}

void TestAudioToolboxAacDecoder::detectsLatmLoasSync()
{
    QVERIFY(AudioToolboxAacDecoder::hasLatmLoasSync(QByteArray::fromHex("56e000"), 0));
    QVERIFY(!AudioToolboxAacDecoder::hasLatmLoasSync(adtsFrame(3, 2, 12), 0));
}

QTEST_GUILESS_MAIN(TestAudioToolboxAacDecoder)
#include "tst_audiotoolboxaacdecoder.moc"
