// Unit tests for RecordingClock — the shared monotonic timeline every capture
// worker reads. Small, pure, deterministic; the natural first unit test.
#include <QtTest>

#include "recorder_engine/recordingclock.h"

class TestRecordingClock : public QObject {
    Q_OBJECT
private slots:
    void invalidBeforeStart();
    void elapsedIsZeroBeforeStart();
    void validAfterStart();
    void elapsedAdvancesMonotonically();
    void restartResetsTowardZero();
};

void TestRecordingClock::invalidBeforeStart() {
    RecordingClock clock;
    QVERIFY(!clock.isValid());
}

void TestRecordingClock::elapsedIsZeroBeforeStart() {
    RecordingClock clock;
    QCOMPARE(clock.elapsedMs(), qint64(0));
}

void TestRecordingClock::validAfterStart() {
    RecordingClock clock;
    clock.start();
    QVERIFY(clock.isValid());
}

void TestRecordingClock::elapsedAdvancesMonotonically() {
    RecordingClock clock;
    clock.start();
    const qint64 t0 = clock.elapsedMs();
    QTest::qWait(60);
    const qint64 t1 = clock.elapsedMs();
    QVERIFY2(t1 >= t0, "clock must never run backwards");
    QVERIFY2(t1 >= 40,
             qPrintable(QStringLiteral("expected >=40ms after 60ms wait, got %1").arg(t1)));
    QVERIFY2(t1 < 5000, "elapsed unexpectedly large");
}

void TestRecordingClock::restartResetsTowardZero() {
    RecordingClock clock;
    clock.start();
    QTest::qWait(50);
    QVERIFY(clock.elapsedMs() >= 30);
    clock.start(); // restart establishes a new Time Zero
    QVERIFY2(clock.elapsedMs() < 30, "start() should reset the timeline");
}

QTEST_GUILESS_MAIN(TestRecordingClock)
#include "tst_recordingclock.moc"
