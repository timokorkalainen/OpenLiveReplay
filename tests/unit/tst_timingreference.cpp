#include <QtTest>
#include "recorder_engine/timing/timingreference.h"
#include "recorder_engine/recordingclock.h"

class TestTimingReference : public QObject {
    Q_OBJECT
private slots:
    void localTierIsLocalMonotonic();
    void localIsNotExternal();
    void nowTracksRecordingClock();
    void nowMsIsNsOver1e6();
};

void TestTimingReference::localTierIsLocalMonotonic() {
    RecordingClock c;
    c.start();
    LocalMonotonicReference ref(&c);
    QCOMPARE(ref.tier(), ReferenceTier::LocalMonotonic);
}
void TestTimingReference::localIsNotExternal() {
    RecordingClock c;
    c.start();
    LocalMonotonicReference ref(&c);
    QVERIFY(!ref.isExternal());
}
void TestTimingReference::nowTracksRecordingClock() {
    RecordingClock c;
    c.start();
    LocalMonotonicReference ref(&c);
    QTest::qWait(20);
    const int64_t refMs = ref.nowSessionMs();
    const int64_t clkMs = c.elapsedMs();
    QVERIFY(qAbs(refMs - clkMs) <= 2); // same clock, sampled microseconds apart
}
void TestTimingReference::nowMsIsNsOver1e6() {
    RecordingClock c;
    c.start();
    LocalMonotonicReference ref(&c);
    QTest::qWait(15);
    QCOMPARE(ref.nowSessionMs(), ref.nowSessionNs() / 1'000'000);
}
QTEST_GUILESS_MAIN(TestTimingReference)
#include "tst_timingreference.moc"
