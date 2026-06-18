#include <QtTest>
#include "recorder_engine/timing/timecodealigner.h"

class TestTimecodeAligner : public QObject {
    Q_OBJECT
private slots:
    void noTimecodeMapsToMinusOne();
    void singleSourceMapsTcToFrame();
    void equalTcSourcesAlign();
    void offsetSourcesReportFrameOffset();
    void resetClears();
};

static int64_t tc100ns(int h, int m, int s, int f, int fps) {
    return Smpte12m::to100ns(Smpte12mTimecode{h, m, s, f, false, true}, fps);
}

void TestTimecodeAligner::noTimecodeMapsToMinusOne() {
    TimecodeAligner a(25);
    QVERIFY(!a.hasTimecode(0));
    QCOMPARE(a.toSessionFrameIndex(0, tc100ns(1, 0, 0, 0, 25)), int64_t(-1));
}
void TestTimecodeAligner::singleSourceMapsTcToFrame() {
    TimecodeAligner a(25);
    // At session frame 100, source 0 carried 01:00:00:00.
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    QVERIFY(a.hasTimecode(0));
    QCOMPARE(a.toSessionFrameIndex(0, tc100ns(1, 0, 0, 0, 25)), int64_t(100));
    // 01:00:00:01 (one frame later in TC) -> session frame 101.
    QCOMPARE(a.toSessionFrameIndex(0, tc100ns(1, 0, 0, 1, 25)), int64_t(101));
}
void TestTimecodeAligner::equalTcSourcesAlign() {
    TimecodeAligner a(25);
    // Both jam-synced: 01:00:00:00 arrived on the SAME session frame 100.
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    a.observe(1, tc100ns(1, 0, 0, 0, 25), 100);
    QVERIFY(a.sourcesAligned(0, 1, 0));
    QCOMPARE(a.frameOffset(0, 1), int64_t(0));
}
void TestTimecodeAligner::offsetSourcesReportFrameOffset() {
    TimecodeAligner a(25);
    // Same TC, but source 1's TC arrived 3 session frames LATER than source 0's.
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    a.observe(1, tc100ns(1, 0, 0, 0, 25), 103);
    QVERIFY(!a.sourcesAligned(0, 1, 0));
    QVERIFY(a.sourcesAligned(0, 1, 3));
    QCOMPARE(a.frameOffset(0, 1), int64_t(-3)); // pull B back 3 frames
}
void TestTimecodeAligner::resetClears() {
    TimecodeAligner a(25);
    a.observe(0, tc100ns(1, 0, 0, 0, 25), 100);
    a.reset();
    QVERIFY(!a.hasTimecode(0));
}
QTEST_GUILESS_MAIN(TestTimecodeAligner)
#include "tst_timecodealigner.moc"
