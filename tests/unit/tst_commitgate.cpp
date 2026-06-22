#include <QtTest>
#include "playback/commitgate.h"

class TestCommitGate : public QObject {
    Q_OBJECT
private slots:
    void passesLivePlayheadWhenNoSeekPending();    // anti-freeze: 1x playback advances
    void holdsCommittedPlayheadWhileSeekPending(); // seek in flight: hold last good
    void snapsToLiveOnceCommitGenerationCatchesUp();
    void bookmarkIgnoresUncoveredTransportJump();
    void visiblePlayheadHoldsBookmarkForUncoveredLiveJump();
    void repositionCommitRequiresSameSeekGeneration();
    void repositionCommitBodyRunsOnlyForOriginalSeek();
    void gpuGenerationInvalidatesOnlyWhenSeekGateIsHeld();
};

// committedGen == seekGen -> no reposition outstanding -> expose the LIVE
// transport playhead so steady forward playback keeps advancing on screen.
void TestCommitGate::passesLivePlayheadWhenNoSeekPending() {
    // seekGen 0, committedGen 0 (equal): live playhead 4000 advances to 4033.
    QCOMPARE(CommitGate::visiblePlayheadMs(4000, /*committed*/ 0, /*cGen*/ 0, /*sGen*/ 0),
             int64_t(4000));
    QCOMPARE(CommitGate::visiblePlayheadMs(4033, /*committed*/ 0, /*cGen*/ 0, /*sGen*/ 0),
             int64_t(4033));
}

// committedGen != seekGen -> a seek is in flight against a not-yet-ready cache
// -> hold the last committed (good) playhead, ignore the live transport value.
void TestCommitGate::holdsCommittedPlayheadWhileSeekPending() {
    // seek bumped to gen 2; commit still at gen 1 covering 5000ms. Transport
    // has jumped to 200ms but the cache is not yet ready -> hold 5000.
    QCOMPARE(CommitGate::visiblePlayheadMs(200, /*committed*/ 5000, /*cGen*/ 1, /*sGen*/ 2),
             int64_t(5000));
}

void TestCommitGate::snapsToLiveOnceCommitGenerationCatchesUp() {
    // Reposition completed: committedGen now equals seekGen (2) and committed
    // the target 200ms. The gate now exposes the live transport playhead again.
    QCOMPARE(CommitGate::visiblePlayheadMs(200, /*committed*/ 200, /*cGen*/ 2, /*sGen*/ 2),
             int64_t(200));
}

void TestCommitGate::bookmarkIgnoresUncoveredTransportJump() {
    // A UI seek can update the transport before PlaybackWorker::seekTo bumps the
    // seek generation. While generations still match, a snapshot must not let
    // that raw transport jump poison the last-visible bookmark unless the cache
    // actually contains a frame at or before that playhead.
    QCOMPARE(CommitGate::bookmarkedVisiblePlayheadMs(
                 /*currentBookmarkMs*/ 5000, /*visiblePlayheadMs*/ 9000,
                 /*cacheCoveredPlayheadMs*/ 5000, /*cacheCovered*/ true,
                 /*committedGen*/ 3, /*seekGen*/ 3),
             int64_t(5000));
    QCOMPARE(CommitGate::bookmarkedVisiblePlayheadMs(
                 /*currentBookmarkMs*/ 5000, /*visiblePlayheadMs*/ 5040,
                 /*cacheCoveredPlayheadMs*/ 5040, /*cacheCovered*/ true,
                 /*committedGen*/ 3, /*seekGen*/ 3),
             int64_t(5040));
}

void TestCommitGate::visiblePlayheadHoldsBookmarkForUncoveredLiveJump() {
    QCOMPARE(CommitGate::cacheGuardedVisiblePlayheadMs(
                 /*visiblePlayheadMs*/ 9000, /*bookmarkedPlayheadMs*/ 5000,
                 /*cacheCovered*/ false, /*committedGen*/ 3, /*seekGen*/ 3),
             int64_t(5000));
    QCOMPARE(CommitGate::cacheGuardedVisiblePlayheadMs(
                 /*visiblePlayheadMs*/ 5040, /*bookmarkedPlayheadMs*/ 5000,
                 /*cacheCovered*/ true, /*committedGen*/ 3, /*seekGen*/ 3),
             int64_t(5040));
    QCOMPARE(CommitGate::cacheGuardedVisiblePlayheadMs(
                 /*visiblePlayheadMs*/ 5000, /*bookmarkedPlayheadMs*/ 4800,
                 /*cacheCovered*/ false, /*committedGen*/ 3, /*seekGen*/ 4),
             int64_t(5000));
}

void TestCommitGate::repositionCommitRequiresSameSeekGeneration() {
    QVERIFY(CommitGate::canCommitReposition(/*startedSeekGen*/ 4, /*currentSeekGen*/ 4,
                                            /*supersededSeekPending*/ false));
    QVERIFY(!CommitGate::canCommitReposition(/*startedSeekGen*/ 4, /*currentSeekGen*/ 5,
                                             /*supersededSeekPending*/ false));
    QVERIFY(!CommitGate::canCommitReposition(/*startedSeekGen*/ 4, /*currentSeekGen*/ 4,
                                             /*supersededSeekPending*/ true));
}

void TestCommitGate::repositionCommitBodyRunsOnlyForOriginalSeek() {
    bool committed = false;
    QVERIFY(CommitGate::commitRepositionIfCurrent(
        /*startedSeekGen*/ 4, /*currentSeekGen*/ 4, /*supersededSeekPending*/ false,
        [&] { committed = true; }));
    QVERIFY(committed);

    committed = false;
    QVERIFY(!CommitGate::commitRepositionIfCurrent(
        /*startedSeekGen*/ 4, /*currentSeekGen*/ 5, /*supersededSeekPending*/ false,
        [&] { committed = true; }));
    QVERIFY(!committed);

    QVERIFY(!CommitGate::commitRepositionIfCurrent(
        /*startedSeekGen*/ 4, /*currentSeekGen*/ 4, /*supersededSeekPending*/ true,
        [&] { committed = true; }));
    QVERIFY(!committed);
}

void TestCommitGate::gpuGenerationInvalidatesOnlyWhenSeekGateIsHeld() {
    QVERIFY(CommitGate::shouldInvalidateGpuGenerationForReposition(
        /*startedSeekGen*/ 7, /*committedGen*/ 6));
    QVERIFY(!CommitGate::shouldInvalidateGpuGenerationForReposition(
        /*startedSeekGen*/ 7, /*committedGen*/ 7));
}

QTEST_MAIN(TestCommitGate)
#include "tst_commitgate.moc"
