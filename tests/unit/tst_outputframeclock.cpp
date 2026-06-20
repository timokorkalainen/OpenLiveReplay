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
    void outputFrameForPlayheadInvertsTheSampleMapping();
    void outputFrameForPlayheadHonorsSlowMotion();
    void outputFrameForPlayheadIsUnsetWhenNotAdvancingForward();
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

void TestOutputFrameClock::outputFrameForPlayheadInvertsTheSampleMapping() {
    // The boundary-fire frame must be the exact inverse of the forward sample
    // mapping, so an armed cut scheduled at outputFrameForPlayheadMs(out) fires when
    // the sampled playhead reaches `out` — frame-perfect, not as-soon-as-staged.
    OutputFrameClock clock(FrameRate::fromFraction(25, 1));
    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.playStartedAtOutputFrame = 50;
    state.playStartedAtPlayheadMs = 1000;

    // Known forward values from oneXForwardAdvancesByFrameDuration, inverted.
    QCOMPARE(clock.outputFrameForPlayheadMs(1000, state), qint64(50));
    QCOMPARE(clock.outputFrameForPlayheadMs(1040, state), qint64(51));
    QCOMPARE(clock.outputFrameForPlayheadMs(2000, state), qint64(75));

    // Full round-trip across a span of frames.
    for (qint64 f = 50; f <= 200; ++f)
        QCOMPARE(
            clock.outputFrameForPlayheadMs(clock.samplePlayheadMsForOutputTick(f, state), state),
            f);
}

void TestOutputFrameClock::outputFrameForPlayheadHonorsSlowMotion() {
    // At 0.5x the playhead advances half as fast, so reaching a given playhead takes
    // twice as many output frames — the inverse must scale by speed.
    OutputFrameClock clock(FrameRate::fromFraction(25, 1)); // 40ms/frame
    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 0.5;
    state.playStartedAtOutputFrame = 0;
    state.playStartedAtPlayheadMs = 0;

    // 1000ms of media at 0.5x = 2000ms wall = 50 output frames (vs 25 at 1x).
    QCOMPARE(clock.outputFrameForPlayheadMs(1000, state), qint64(50));
    // Round-trip at slow-mo.
    for (qint64 f = 0; f <= 200; ++f)
        QCOMPARE(
            clock.outputFrameForPlayheadMs(clock.samplePlayheadMsForOutputTick(f, state), state),
            f);

    // And faster than realtime (2x): half the frames (960ms / (2 * 40ms) = 12).
    state.speed = 2.0;
    QCOMPARE(clock.outputFrameForPlayheadMs(960, state), qint64(12));
}

void TestOutputFrameClock::outputFrameForPlayheadIsUnsetWhenNotAdvancingForward() {
    OutputFrameClock clock(FrameRate::fromFraction(25, 1));
    PlaybackStateSnapshot state;
    state.playStartedAtOutputFrame = 10;
    state.playStartedAtPlayheadMs = 500;

    state.playing = false; // paused
    state.speed = 1.0;
    QCOMPARE(clock.outputFrameForPlayheadMs(800, state), qint64(-1));

    state.playing = true;
    state.speed = -1.0; // reverse — boundary cuts only meaningful forward
    QCOMPARE(clock.outputFrameForPlayheadMs(800, state), qint64(-1));

    state.speed = 0.0; // halted
    QCOMPARE(clock.outputFrameForPlayheadMs(800, state), qint64(-1));
}

QTEST_GUILESS_MAIN(TestOutputFrameClock)
#include "tst_outputframeclock.moc"
