#include <QtTest>

#include "playback/output/outputframeclock.h"

class TestOutputFrameClock : public QObject {
    Q_OBJECT
private slots:
    void frameRateRejectsInvalidValues();
    void frameIndexToPlayheadMsUsesRationalMath();
    void pausedPlayheadRepeatsSameSourceFrame();
    void oneXForwardAdvancesByFrameDuration();
    void shuttleCanMoveBackwardWithoutStoppingOutputTicks();
};

void TestOutputFrameClock::frameRateRejectsInvalidValues() {
    QVERIFY(!FrameRate::fromFraction(0, 1).isValid());
    QVERIFY(!FrameRate::fromFraction(30000, 0).isValid());
    QVERIFY(FrameRate::fromFraction(30000, 1001).isValid());
}

void TestOutputFrameClock::frameIndexToPlayheadMsUsesRationalMath() {
    const OutputFrameClock clock(FrameRate::fromFraction(30000, 1001));
    QCOMPARE(clock.frameIndexToPlayheadMs(0), qint64(0));
    QCOMPARE(clock.frameIndexToPlayheadMs(30), qint64(1001));
    QCOMPARE(clock.frameIndexToPlayheadMs(60), qint64(2002));
}

void TestOutputFrameClock::pausedPlayheadRepeatsSameSourceFrame() {
    OutputFrameClock clock(FrameRate::fromFraction(30, 1));
    PlaybackStateSnapshot state;
    state.playheadMs = 412 * 1000 / 30;
    state.playing = false;
    state.speed = 1.0;

    QCOMPARE(clock.samplePlayheadMsForOutputTick(1000, state), state.playheadMs);
    QCOMPARE(clock.samplePlayheadMsForOutputTick(1001, state), state.playheadMs);
    QCOMPARE(clock.samplePlayheadMsForOutputTick(1002, state), state.playheadMs);
}

void TestOutputFrameClock::oneXForwardAdvancesByFrameDuration() {
    OutputFrameClock clock(FrameRate::fromFraction(25, 1));
    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = true;
    state.speed = 1.0;
    state.playStartedAtOutputFrame = 50;
    state.playStartedAtPlayheadMs = 1000;

    QCOMPARE(clock.samplePlayheadMsForOutputTick(50, state), qint64(1000));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(51, state), qint64(1040));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(75, state), qint64(2000));
}

void TestOutputFrameClock::shuttleCanMoveBackwardWithoutStoppingOutputTicks() {
    OutputFrameClock clock(FrameRate::fromFraction(50, 1));
    PlaybackStateSnapshot state;
    state.playheadMs = 5000;
    state.playing = true;
    state.speed = -2.0;
    state.playStartedAtOutputFrame = 10;
    state.playStartedAtPlayheadMs = 5000;

    QCOMPARE(clock.samplePlayheadMsForOutputTick(10, state), qint64(5000));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(11, state), qint64(4960));
    QCOMPARE(clock.samplePlayheadMsForOutputTick(12, state), qint64(4920));
}

QTEST_GUILESS_MAIN(TestOutputFrameClock)
#include "tst_outputframeclock.moc"
