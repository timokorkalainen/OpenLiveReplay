// Unit tests for the pure record-gate decisions (hard block + soft warn).
#include <QtTest>

#include "recorder_engine/benchmark/recordgate.h"

class TestRecordGate : public QObject {
    Q_OBJECT
private slots:
    void hardBlockOnlyForH264WithoutHardware();
    void softWarnOnlyWhenConfiguredExceedsSafe();
    void blockReasonIsNonEmptyForH264();
};

void TestRecordGate::hardBlockOnlyForH264WithoutHardware() {
    QVERIFY(recordCodecUnavailable(VideoCodecChoice::H264Hardware, false));   // block
    QVERIFY(!recordCodecUnavailable(VideoCodecChoice::H264Hardware, true));   // hw present
    QVERIFY(!recordCodecUnavailable(VideoCodecChoice::Mpeg2Software, false)); // mpeg2 always ok
    QVERIFY(!recordCodecUnavailable(VideoCodecChoice::Mpeg2Software, true));
}

void TestRecordGate::softWarnOnlyWhenConfiguredExceedsSafe() {
    QVERIFY(feedCountExceedsSafe(10, 8));   // over -> warn
    QVERIFY(!feedCountExceedsSafe(8, 8));   // equal -> no warn
    QVERIFY(!feedCountExceedsSafe(4, 8));   // under -> no warn
    QVERIFY(!feedCountExceedsSafe(10, 0));  // not benchmarked -> no warn
    QVERIFY(!feedCountExceedsSafe(10, -1)); // unknown -> no warn
}

void TestRecordGate::blockReasonIsNonEmptyForH264() {
    QVERIFY(!recordCodecBlockReason(VideoCodecChoice::H264Hardware).isEmpty());
}

QTEST_GUILESS_MAIN(TestRecordGate)
#include "tst_recordgate.moc"
