#include <QtTest>

#include "playback/livefollowpolicy.h"

class TestLiveFollowPolicy : public QObject {
    Q_OBJECT
private slots:
    void waitsUntilLiveBufferExists();
    void ignoresTinyCorrections();
    void resetsOutputClockForSmallCorrections();
    void seeksWorkerForLargeDiscontinuities();
};

void TestLiveFollowPolicy::waitsUntilLiveBufferExists() {
    const LiveFollowCorrection correction = planLiveFollowCorrection(
        /*followLive=*/true, /*playing=*/true, /*liveEdgeMs=*/900, /*liveBufferMs=*/1000,
        /*currentMs=*/900, /*frameDurationMs=*/20);

    QVERIFY(!correction.adjustTransport);
    QVERIFY(!correction.resetOutputClock);
    QVERIFY(!correction.seekWorker);
}

void TestLiveFollowPolicy::ignoresTinyCorrections() {
    const LiveFollowCorrection correction = planLiveFollowCorrection(
        /*followLive=*/true, /*playing=*/true, /*liveEdgeMs=*/5000, /*liveBufferMs=*/1000,
        /*currentMs=*/3960, /*frameDurationMs=*/20);

    QVERIFY(!correction.adjustTransport);
    QVERIFY(!correction.resetOutputClock);
    QVERIFY(!correction.seekWorker);
}

void TestLiveFollowPolicy::resetsOutputClockForSmallCorrections() {
    const LiveFollowCorrection correction = planLiveFollowCorrection(
        /*followLive=*/true, /*playing=*/true, /*liveEdgeMs=*/5000, /*liveBufferMs=*/1000,
        /*currentMs=*/3880, /*frameDurationMs=*/20);

    QVERIFY(correction.adjustTransport);
    QCOMPARE(correction.targetMs, qint64(4000));
    QVERIFY(correction.resetOutputClock);
    QVERIFY(!correction.seekWorker);
    QCOMPARE(correction.directionHint, 1);
}

void TestLiveFollowPolicy::seeksWorkerForLargeDiscontinuities() {
    const LiveFollowCorrection correction = planLiveFollowCorrection(
        /*followLive=*/true, /*playing=*/true, /*liveEdgeMs=*/5000, /*liveBufferMs=*/1000,
        /*currentMs=*/900, /*frameDurationMs=*/20);

    QVERIFY(correction.adjustTransport);
    QCOMPARE(correction.targetMs, qint64(4000));
    QVERIFY(correction.seekWorker);
    QVERIFY(!correction.resetOutputClock);
    QCOMPARE(correction.directionHint, 1);
}

QTEST_GUILESS_MAIN(TestLiveFollowPolicy)
#include "tst_livefollowpolicy.moc"
