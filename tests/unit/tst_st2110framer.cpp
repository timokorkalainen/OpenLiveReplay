// St2110FrameFramer packs encoded video essence with the RTP-style fields the
// DeckLink ST2110 IP output expects. It is SDK-independent so timestamp
// progression and marker metadata are tested without the IP SDK.
#include <QtTest>

#include "playback/output/iotargets/st2110framer.h"

class TestSt2110Framer : public QObject {
    Q_OBJECT
private slots:
    void rtpTimestampAdvancesByMediaClock();
    void framesEssenceWithMarkerAndSsrc();
    void programmeTimecodeDrivesTimestampWhenPresent();
    void emptyEssenceIsInvalid();
};

void TestSt2110Framer::rtpTimestampAdvancesByMediaClock() {
    St2110FrameFramer framer(FrameRate::fromFraction(60, 1), 0xABCD1234u);

    QCOMPARE(framer.rtpTimestampForFrame(0), quint32(0));
    QCOMPARE(framer.rtpTimestampForFrame(1), quint32(1500));
    QCOMPARE(framer.rtpTimestampForFrame(4), quint32(6000));
}

void TestSt2110Framer::framesEssenceWithMarkerAndSsrc() {
    St2110FrameFramer framer(FrameRate::fromFraction(60, 1), 0xABCD1234u,
                             /*payloadType=*/97);
    const QByteArray essence("\x00\x00\x00\x01\x67payload", 13);
    const St2110VideoFrame frame = framer.frameVideo(essence, /*outputFrameIndex=*/2,
                                                     /*programmeTimecode100ns=*/-1);

    QCOMPARE(frame.essence, essence);
    QCOMPARE(frame.ssrc, quint32(0xABCD1234u));
    QCOMPARE(frame.payloadType, quint8(97));
    QVERIFY(frame.markerLast);
    QCOMPARE(frame.rtpTimestamp90k, quint32(3000));
}

void TestSt2110Framer::programmeTimecodeDrivesTimestampWhenPresent() {
    St2110FrameFramer framer(FrameRate::fromFraction(60, 1), 1u);
    const St2110VideoFrame frame = framer.frameVideo(QByteArray("abc"), /*outputFrameIndex=*/2,
                                                     /*programmeTimecode100ns=*/10000000);

    QCOMPARE(frame.rtpTimestamp90k, quint32(90000));
}

void TestSt2110Framer::emptyEssenceIsInvalid() {
    St2110FrameFramer framer(FrameRate::fromFraction(50, 1), 1u);
    const St2110VideoFrame frame = framer.frameVideo(QByteArray(), 0, -1);

    QVERIFY(frame.essence.isEmpty());
}

QTEST_GUILESS_MAIN(TestSt2110Framer)
#include "tst_st2110framer.moc"
