#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"

class TestRtmpHealth : public QObject {
    Q_OBJECT
private slots:
    void freshDecodingIsGreen();
    void stallIsRed();
    void briefStallIsAmber();
    void decodeFailureIsAmber();
    void sustainedDecodeFailureIsRed();
    void staleKeyframeIsAmber();
    void counterResetIsGreen();
};

static IngestStats rtmpSnap(quint64 bytes, qint64 lastPktAge, qint64 keyframeAge,
                            quint64 decodeFails) {
    IngestStats s;
    s.kind = IngestStatsKind::Rtmp;
    s.bytesTotal = bytes;
    s.lastPacketAgeMs = lastPktAge;
    s.keyframeAgeMs = keyframeAge;
    s.decodeFailures = decodeFails;
    return s;
}

void TestRtmpHealth::freshDecodingIsGreen() {
    QCOMPARE(rtmpHealth(rtmpSnap(100000, 50, 500, 0), rtmpSnap(200000, 50, 500, 0)),
             SourceHealth::Green);
}
void TestRtmpHealth::stallIsRed() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 0), rtmpSnap(200000, 3500, 4000, 0)),
             SourceHealth::Red);
}
void TestRtmpHealth::briefStallIsAmber() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 0), rtmpSnap(210000, 1500, 700, 0)),
             SourceHealth::Amber);
}
void TestRtmpHealth::decodeFailureIsAmber() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 3), rtmpSnap(300000, 50, 500, 4)),
             SourceHealth::Amber);
}
void TestRtmpHealth::sustainedDecodeFailureIsRed() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 500, 3), rtmpSnap(200000, 50, 500, 8)),
             SourceHealth::Red);
}
void TestRtmpHealth::staleKeyframeIsAmber() {
    QCOMPARE(rtmpHealth(rtmpSnap(200000, 50, 5500, 0), rtmpSnap(300000, 50, 6000, 0)),
             SourceHealth::Amber);
}
void TestRtmpHealth::counterResetIsGreen() {
    QCOMPARE(rtmpHealth(rtmpSnap(5000000, 50, 500, 9), rtmpSnap(100000, 50, 500, 0)),
             SourceHealth::Green);
}

QTEST_GUILESS_MAIN(TestRtmpHealth)
#include "tst_rtmp_health.moc"
