#include <QtTest>
#include "playback/cutschedule.h"

class TestCutSchedule : public QObject {
    Q_OBJECT
private slots:
    void leadFramesAddedToNextIndex();
    void zeroLeadSchedulesNextFrame();
    void framesForLeadMsRoundsUp();
    void shouldFireExactlyAtScheduledFrame();
    void shouldFireWhenTickOvershoots();
    void shouldNotFireBeforeScheduledFrame();
    void playheadAccountsForOvershoot();
};

void TestCutSchedule::leadFramesAddedToNextIndex() {
    QCOMPARE(CutSchedule::outputFrameForCut(100, 3), qint64(103));
}

void TestCutSchedule::zeroLeadSchedulesNextFrame() {
    // Lead 0 must still land on a frame the dispatcher has not yet emitted.
    QCOMPARE(CutSchedule::outputFrameForCut(100, 0), qint64(100));
}

void TestCutSchedule::framesForLeadMsRoundsUp() {
    // 25 fps -> 40 ms/frame. 50 ms -> 2 frames (ceil).
    QCOMPARE(CutSchedule::framesForLeadMs(50, 25.0), qint64(2));
    // 41 ms -> ceil(1.025) = 2.
    QCOMPARE(CutSchedule::framesForLeadMs(41, 25.0), qint64(2));
    // 40 ms -> exactly 1.
    QCOMPARE(CutSchedule::framesForLeadMs(40, 25.0), qint64(1));
    // 0 ms -> 0 frames.
    QCOMPARE(CutSchedule::framesForLeadMs(0, 25.0), qint64(0));
}

void TestCutSchedule::shouldFireExactlyAtScheduledFrame() {
    QVERIFY(CutSchedule::shouldFireAt(103, 103));
}

void TestCutSchedule::shouldFireWhenTickOvershoots() {
    // Dispatcher may skip an index under load; fire on >= to avoid a missed cut.
    QVERIFY(CutSchedule::shouldFireAt(105, 103));
}

void TestCutSchedule::shouldNotFireBeforeScheduledFrame() {
    QVERIFY(!CutSchedule::shouldFireAt(102, 103));
}

void TestCutSchedule::playheadAccountsForOvershoot() {
    // Fired exactly on schedule -> playhead == targetMs.
    QCOMPARE(CutSchedule::playheadAfterCut(2000, 103, 103, 25.0), qint64(2000));
    // Fired one frame late at 25fps (40ms/frame) -> targetMs + 40.
    QCOMPARE(CutSchedule::playheadAfterCut(2000, 104, 103, 25.0), qint64(2040));
    // Negative/zero fps guard -> targetMs unchanged.
    QCOMPARE(CutSchedule::playheadAfterCut(2000, 104, 103, 0.0), qint64(2000));
}

QTEST_MAIN(TestCutSchedule)
#include "tst_cutschedule.moc"
