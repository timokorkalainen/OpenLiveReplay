#include <QtTest>

#include "recorder_engine/framerate.h"
#include "recorder_engine/heartbeat.h"

class TestHeartbeat : public QObject {
    Q_OBJECT
private slots:
    void notAdvancedReturnsEmpty();
    void mapsElapsedToFrameAtStart();
    void singleFrameAdvance();
    void lateTickCappedAtMaxPerTick();
    void backlogSkipWhenFarBehind();
    void fpsZeroReturnsEmpty();
    void rate2997Span();
};

void TestHeartbeat::notAdvancedReturnsEmpty() {
    // elapsed 0 ms, no frames yet: derivedFrame 0 <= lastFrame 0 -> empty (to < from).
    const FrameSpan s = heartbeatFrameSpan(0, FrameRate{30, 1}, 0, 8, 30);
    QVERIFY(s.to < s.from);
}

void TestHeartbeat::mapsElapsedToFrameAtStart() {
    // 100 ms @30fps -> frame 3; from a cold start (lastFrame 0) emit 1..3 (catch-up).
    const FrameSpan s = heartbeatFrameSpan(100, FrameRate{30, 1}, 0, 8, 30);
    QCOMPARE(s.from, qint64(1));
    QCOMPARE(s.to, qint64(3));
}

void TestHeartbeat::singleFrameAdvance() {
    // 100 ms @30fps -> frame 3; already at frame 2 -> emit just frame 3.
    const FrameSpan s = heartbeatFrameSpan(100, FrameRate{30, 1}, 2, 8, 30);
    QCOMPARE(s.from, qint64(3));
    QCOMPARE(s.to, qint64(3));
}

void TestHeartbeat::lateTickCappedAtMaxPerTick() {
    // 1000 ms @30fps -> frame 30; cold start would want 1..30 but the burst is
    // capped at maxPerTick(8) -> 1..8 (remainder drains on later ticks). Not far
    // enough behind to trigger the backlog skip (29 < maxBacklog 30).
    const FrameSpan s = heartbeatFrameSpan(1000, FrameRate{30, 1}, 0, 8, 30);
    QCOMPARE(s.from, qint64(1));
    QCOMPARE(s.to, qint64(8));
}

void TestHeartbeat::backlogSkipWhenFarBehind() {
    // 2000 ms @30fps -> frame 60, > 1 s behind (60-1 >= maxBacklog 30): skip ahead
    // to frame 31 and emit 31..38 (capped at 8) so recording resumes near real time.
    const FrameSpan s = heartbeatFrameSpan(2000, FrameRate{30, 1}, 0, 8, 30);
    QCOMPARE(s.from, qint64(31));
    QCOMPARE(s.to, qint64(38));
}

void TestHeartbeat::fpsZeroReturnsEmpty() {
    const FrameSpan s = heartbeatFrameSpan(1000, FrameRate{0, 1}, 0, 8, 30);
    QVERIFY(s.to < s.from);
}

void TestHeartbeat::rate2997Span() {
    // 1001 ms @29.97 -> frame 30 exactly; lastFrame 29 -> emit just frame 30.
    const FrameSpan s = heartbeatFrameSpan(1001, FrameRate{30000, 1001}, 29, 8, 30);
    QCOMPARE(s.from, qint64(30));
    QCOMPARE(s.to, qint64(30));
}

QTEST_GUILESS_MAIN(TestHeartbeat)
#include "tst_heartbeat.moc"
