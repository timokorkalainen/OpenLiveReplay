#include <QtTest>
#include "playback/commitgate.h"

class TestCommitGate : public QObject {
    Q_OBJECT
private slots:
    void passesLivePlayheadWhenNoSeekPending();    // anti-freeze: 1x playback advances
    void holdsCommittedPlayheadWhileSeekPending(); // seek in flight: hold last good
    void snapsToLiveOnceCommitGenerationCatchesUp();
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

QTEST_MAIN(TestCommitGate)
#include "tst_commitgate.moc"
