#include <QtTest>

#include "playback/playlistplayout.h"

static ReplayEntry entry(qint64 inMs, qint64 outMs, double speed = 1.0) {
    ReplayEntry e;
    e.clipPath = QStringLiteral("clip.mkv");
    e.inMs = inMs;
    e.outMs = outMs;
    e.speed = speed;
    return e;
}

class TestPlaylistPlayout : public QObject {
    Q_OBJECT
private slots:
    void armsBoundaryOnceWhenWithinTheLead();
    void advancesThroughEntriesOnEachBoundaryFire();
    void finalEntryArmsNothingAndPlaysForward();
    void leadScalesWithSpeed();
    void inactiveUntilStarted();
};

void TestPlaylistPlayout::armsBoundaryOnceWhenWithinTheLead() {
    PlaylistPlayout p;
    p.start({entry(1000, 5000, 1.0), entry(20000, 24000, 1.0)}, 0);
    QVERIFY(p.active());
    QCOMPARE(p.currentIndex(), 0);

    const qint64 lead = 1500; // ms realtime

    // Far from the out-point: nothing armed.
    QVERIFY(!p.evaluate(2000, 1.0, lead).valid);
    QVERIFY(!p.evaluate(3000, 1.0, lead).valid); // out(5000) - lead(1500) = 3500, 3000 < 3500

    // Within the lead window: arm exactly once, to the next entry's in-point.
    const auto b = p.evaluate(3600, 1.0, lead);
    QVERIFY(b.valid);
    QCOMPARE(b.fireAtMs, qint64(5000));  // current out-point
    QCOMPARE(b.targetMs, qint64(20000)); // next in-point
    QCOMPARE(b.targetSpeed, 1.0);

    // Re-evaluating the same boundary does not re-arm.
    QVERIFY(!p.evaluate(3700, 1.0, lead).valid);
    QVERIFY(!p.evaluate(4900, 1.0, lead).valid);
}

void TestPlaylistPlayout::advancesThroughEntriesOnEachBoundaryFire() {
    PlaylistPlayout p;
    p.start({entry(0, 2000, 1.0), entry(10000, 12000, 0.5), entry(20000, 22000, 2.0)}, 0);

    QVERIFY(p.evaluate(1800, 1.0, 1500).valid); // arm boundary 0->1
    auto e1 = p.onBoundaryFired();
    QVERIFY(e1.has_value());
    QCOMPARE(p.currentIndex(), 1);
    QCOMPARE(e1->inMs, qint64(10000));
    QCOMPARE(e1->speed, 0.5); // caller applies the next entry's (slow-mo) speed

    // Boundary 1 is armed against entry 1's out-point at its 0.5x speed.
    const auto b1 = p.evaluate(11500, 0.5, 1500);
    QVERIFY(b1.valid);
    QCOMPARE(b1.fireAtMs, qint64(12000));
    QCOMPARE(b1.targetMs, qint64(20000));
    QCOMPARE(b1.targetSpeed, 2.0);

    auto e2 = p.onBoundaryFired();
    QVERIFY(e2.has_value());
    QCOMPARE(p.currentIndex(), 2);
    QVERIFY(p.onFinalEntry());
}

void TestPlaylistPlayout::finalEntryArmsNothingAndPlaysForward() {
    PlaylistPlayout p;
    p.start({entry(0, 2000, 1.0), entry(10000, 12000, 1.0)}, 1); // start ON the last entry
    QVERIFY(p.onFinalEntry());
    // Even right at / past its out-point, no boundary is armed — playback flows forward.
    QVERIFY(!p.evaluate(11999, 1.0, 1500).valid);
    QVERIFY(!p.evaluate(12000, 1.0, 1500).valid);
    QVERIFY(!p.evaluate(13000, 1.0, 1500).valid);
    // No next entry to advance to.
    QVERIFY(!p.onBoundaryFired().has_value());
}

void TestPlaylistPlayout::leadScalesWithSpeed() {
    // The clip-time arm distance must scale with speed so the realtime pre-roll
    // headroom is constant. out=5000, lead=1000ms realtime.
    PlaylistPlayout p2x;
    p2x.start({entry(0, 5000, 2.0), entry(9000, 9500, 1.0)}, 0);
    // At 2x, arm when clip-distance <= 2000 (out - 2000 = 3000).
    QVERIFY(!p2x.evaluate(2900, 2.0, 1000).valid);
    QVERIFY(p2x.evaluate(3100, 2.0, 1000).valid);

    PlaylistPlayout pHalf;
    pHalf.start({entry(0, 5000, 0.5), entry(9000, 9500, 1.0)}, 0);
    // At 0.5x, arm when clip-distance <= 500 (out - 500 = 4500) — much later in clip
    // time, but the same realtime headroom since playback is slow.
    QVERIFY(!pHalf.evaluate(4400, 0.5, 1000).valid);
    QVERIFY(pHalf.evaluate(4600, 0.5, 1000).valid);
}

void TestPlaylistPlayout::inactiveUntilStarted() {
    PlaylistPlayout p;
    QVERIFY(!p.active());
    QVERIFY(!p.evaluate(1000, 1.0, 1500).valid);
    QVERIFY(!p.onBoundaryFired().has_value());

    p.start({}, 0); // empty playlist
    QVERIFY(!p.active());

    p.start({entry(0, 1000)}, 5); // out-of-range index
    QVERIFY(!p.active());

    p.start({entry(0, 1000)}, 0);
    QVERIFY(p.active());
    p.stop();
    QVERIFY(!p.active());
}

QTEST_GUILESS_MAIN(TestPlaylistPlayout)
#include "tst_playlistplayout.moc"
