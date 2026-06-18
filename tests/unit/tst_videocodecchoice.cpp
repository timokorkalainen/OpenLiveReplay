// Unit tests for VideoCodecChoice enum <-> string helpers.
#include <QtTest>

#include "recorder_engine/codec/videocodecchoice.h"

class TestVideoCodecChoice : public QObject {
    Q_OBJECT
private slots:
    void toStringMapsBothValues();
    void fromStringMapsKnownValues();
    void fromStringUsesFallbackForUnknown();
    void roundTrips();
};

void TestVideoCodecChoice::toStringMapsBothValues() {
    QCOMPARE(videoCodecToString(VideoCodecChoice::Mpeg2Software), QStringLiteral("mpeg2"));
    QCOMPARE(videoCodecToString(VideoCodecChoice::H264Hardware), QStringLiteral("h264"));
}

void TestVideoCodecChoice::fromStringMapsKnownValues() {
    QCOMPARE(videoCodecFromString(QStringLiteral("mpeg2")), VideoCodecChoice::Mpeg2Software);
    QCOMPARE(videoCodecFromString(QStringLiteral("h264")), VideoCodecChoice::H264Hardware);
}

void TestVideoCodecChoice::fromStringUsesFallbackForUnknown() {
    QCOMPARE(videoCodecFromString(QStringLiteral("")), VideoCodecChoice::Mpeg2Software);
    QCOMPARE(videoCodecFromString(QStringLiteral("vp9")), VideoCodecChoice::Mpeg2Software);
    QCOMPARE(videoCodecFromString(QStringLiteral("vp9"), VideoCodecChoice::H264Hardware),
             VideoCodecChoice::H264Hardware);
}

void TestVideoCodecChoice::roundTrips() {
    for (auto c : {VideoCodecChoice::Mpeg2Software, VideoCodecChoice::H264Hardware})
        QCOMPARE(videoCodecFromString(videoCodecToString(c)), c);
}

QTEST_GUILESS_MAIN(TestVideoCodecChoice)
#include "tst_videocodecchoice.moc"
