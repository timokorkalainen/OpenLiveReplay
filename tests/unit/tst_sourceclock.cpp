#include <QtTest>

#include "recorder_engine/timing/sourceclock.h"

#include <cmath>

class TestSourceClock : public QObject {
    Q_OBJECT

private slots:
    void startsUnlocked();
    void authorityEstablishesAnchor();
    void authorityJumpReanchors();
    void discontinuityReanchors();
    void followerDoesNotReanchorExistingAnchor();
    void followerDoesNotPollutePpm();
    void measuresPpm();
    void preserves90kAnchorMath();
};

void TestSourceClock::startsUnlocked() {
    AnchoredSourceClock clock(ClockQuality::Pcr, 90);
    QVERIFY(!clock.locked());
    QCOMPARE(clock.toSessionMs(90000), int64_t(-1));
}

void TestSourceClock::authorityEstablishesAnchor() {
    AnchoredSourceClock clock(ClockQuality::Pcr);
    clock.observe(1000, 5000, false);
    QVERIFY(clock.locked());
    QCOMPARE(clock.toSessionMs(1000), int64_t(5000));
    QCOMPARE(clock.toSessionMs(1100), int64_t(5100));
    QCOMPARE(clock.quality(), ClockQuality::Pcr);
}

void TestSourceClock::authorityJumpReanchors() {
    AnchoredSourceClock clock(ClockQuality::FlvPll);
    clock.observe(1000, 5000, false);
    clock.observe(9000, 6000, false);
    QCOMPARE(clock.toSessionMs(9000), int64_t(6000));
}

void TestSourceClock::discontinuityReanchors() {
    AnchoredSourceClock clock(ClockQuality::FlvPll);
    clock.observe(1000, 5000, false);
    clock.observe(1200, 5200, true);
    QCOMPARE(clock.toSessionMs(1200), int64_t(5200));
}

void TestSourceClock::followerDoesNotReanchorExistingAnchor() {
    AnchoredSourceClock clock(ClockQuality::FlvPll);
    clock.observe(1000, 5000, false);
    clock.observe(9000, 99999, false, ClockObservationRole::Follower);
    QCOMPARE(clock.toSessionMs(9000), int64_t(13000));
}

void TestSourceClock::followerDoesNotPollutePpm() {
    AnchoredSourceClock clock(ClockQuality::FlvPll);
    for (int i = 0; i < 300; ++i) {
        clock.observe(int64_t(i) * 1000, int64_t(i) * 1000, false);
        clock.observe(900000 + int64_t(i) * 1000, 1000 + int64_t(i) * 1000, false,
                      ClockObservationRole::Follower);
    }
    QVERIFY(std::abs(clock.ppm()) < 1.0);
}

void TestSourceClock::measuresPpm() {
    AnchoredSourceClock clock(ClockQuality::FlvPll);
    for (int i = 0; i < 300; ++i) {
        const int64_t sessionMs = int64_t(i) * 1000;
        const int64_t senderMs = int64_t(std::llround(double(i) * 1000.0 * 1.0002));
        clock.observe(senderMs, sessionMs, false);
    }
    QVERIFY(std::abs(clock.ppm() - 200.0) < 5.0);
}

void TestSourceClock::preserves90kAnchorMath() {
    AnchoredSourceClock clock(ClockQuality::Pcr, 90);
    clock.observe(90001, 1000, false);
    QCOMPARE(clock.toSessionMs(108001), int64_t(1200));
}

QTEST_GUILESS_MAIN(TestSourceClock)
#include "tst_sourceclock.moc"
