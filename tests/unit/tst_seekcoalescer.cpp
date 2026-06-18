#include <QtTest>

#include "playback/seekcoalescer.h"

class TestSeekCoalescer : public QObject {
    Q_OBJECT
private slots:
    void firstOfferSeeksImmediately();
    void burstAfterFirstDefersAndKeepsOnlyLatest();
    void commitClearsPendingUntilNextBurst();
    void resetReArmsImmediateSeek();
};

void TestSeekCoalescer::firstOfferSeeksImmediately() {
    SeekCoalescer c;
    QVERIFY(c.offer(100)); // first move → seek now
}

void TestSeekCoalescer::burstAfterFirstDefersAndKeepsOnlyLatest() {
    SeekCoalescer c;
    QVERIFY(c.offer(100));  // immediate
    QVERIFY(!c.offer(110)); // deferred
    QVERIFY(!c.offer(120)); // deferred, supersedes 110
    bool has = false;
    QCOMPARE(c.takePending(has), int64_t(120));
    QVERIFY(has);
    // Drained: nothing left until the next deferred offer.
    bool has2 = true;
    c.takePending(has2);
    QVERIFY(!has2);
}

void TestSeekCoalescer::commitClearsPendingUntilNextBurst() {
    SeekCoalescer c;
    QVERIFY(c.offer(100));
    QVERIFY(!c.offer(110));
    bool has = false;
    QCOMPARE(c.takePending(has), int64_t(110));
    QVERIFY(has);
    // After draining, a NEW offer is still deferred (the gesture is ongoing),
    // not immediate — only reset() re-arms the immediate path.
    QVERIFY(!c.offer(130));
}

void TestSeekCoalescer::resetReArmsImmediateSeek() {
    SeekCoalescer c;
    QVERIFY(c.offer(100));
    QVERIFY(!c.offer(110));
    c.reset();             // release: gesture over
    QVERIFY(c.offer(200)); // next gesture's first move is immediate again
}

QTEST_GUILESS_MAIN(TestSeekCoalescer)
#include "tst_seekcoalescer.moc"
