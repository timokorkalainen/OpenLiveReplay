#include <QtTest>
#include "recorder_engine/timing/ptpservo.h"
#include "recorder_engine/timing/iptpclient.h"

// Build a synthetic two-way PTP exchange where the master leads the slave by a
// known offset O and the path is symmetric with one-way delay D. Per IEEE 1588:
//   t2 = t1 + D + O   (Sync: master->slave, slave clock ahead by O)
//   t4 = t3 + D - O   (Delay_Req: slave->master, converted back to master time)
// so meanPathDelay == D and offset == O exactly.
static PtpExchange makeExchange(int64_t t1, int64_t t3, int64_t offsetO, int64_t delayD) {
    PtpExchange ex;
    ex.t1 = t1;
    ex.t2 = t1 + delayD + offsetO;
    ex.t3 = t3;
    ex.t4 = t3 + delayD - offsetO;
    ex.valid = true;
    return ex;
}

class TestPtpServo : public QObject {
    Q_OBJECT
private slots:
    void unlockedBeforeMinExchanges();
    void locksAfterMinExchanges();
    void offsetAndDelayConverge();
    void masterNsFromLocalIsLocalMinusOffset();
    void resetUnlocks();
    void invalidExchangeIgnored();
    void noisySampleDoesNotUnlockOrJerk();
    void tracksSteadyOffset();
    void tracksStepInOffset();
    void tracksLargeStepViaReArm();
    void recoversLargeAbsoluteOffsetWithoutOverflow();
};

void TestPtpServo::unlockedBeforeMinExchanges() {
    PtpServo servo(8);
    for (int i = 0; i < 7; ++i) {
        servo.observe(makeExchange(1000 * i, 1000 * i + 500, /*O*/ 4000, /*D*/ 60000));
        QVERIFY(!servo.locked());
        QCOMPARE(servo.offsetNs(), int64_t(0));        // 0 until locked
        QCOMPARE(servo.meanPathDelayNs(), int64_t(0)); // 0 until locked
    }
}

void TestPtpServo::locksAfterMinExchanges() {
    PtpServo servo(8);
    for (int i = 0; i < 8; ++i) {
        servo.observe(makeExchange(1000 * i, 1000 * i + 500, 4000, 60000));
    }
    QVERIFY(servo.locked());
}

void TestPtpServo::offsetAndDelayConverge() {
    const int64_t O = 4000;  // master leads slave by 4 us
    const int64_t D = 60000; // 60 us one-way path
    PtpServo servo(8);
    for (int i = 0; i < 16; ++i) {
        servo.observe(makeExchange(1000 * i, 1000 * i + 500, O, D));
    }
    QVERIFY(servo.locked());
    QVERIFY(qAbs(servo.offsetNs() - O) <= 1);
    QVERIFY(qAbs(servo.meanPathDelayNs() - D) <= 1);
}

void TestPtpServo::masterNsFromLocalIsLocalMinusOffset() {
    const int64_t O = 4000;
    PtpServo servo(8);
    for (int i = 0; i < 12; ++i) {
        servo.observe(makeExchange(1000 * i, 1000 * i + 500, O, 60000));
    }
    QVERIFY(servo.locked());
    const int64_t localNs = 5'000'000'000LL;
    QCOMPARE(servo.masterNsFromLocal(localNs), localNs - servo.offsetNs());
}

void TestPtpServo::resetUnlocks() {
    PtpServo servo(8);
    for (int i = 0; i < 10; ++i) {
        servo.observe(makeExchange(1000 * i, 1000 * i + 500, 4000, 60000));
    }
    QVERIFY(servo.locked());
    servo.reset();
    QVERIFY(!servo.locked());
    QCOMPARE(servo.offsetNs(), int64_t(0));
    QCOMPARE(servo.meanPathDelayNs(), int64_t(0));
}

void TestPtpServo::invalidExchangeIgnored() {
    PtpServo servo(4);
    for (int i = 0; i < 4; ++i) {
        PtpExchange bad = makeExchange(1000 * i, 1000 * i + 500, 4000, 60000);
        bad.valid = false;
        servo.observe(bad);
    }
    QVERIFY(!servo.locked()); // invalid samples never count toward lock
}

void TestPtpServo::noisySampleDoesNotUnlockOrJerk() {
    const int64_t O = 4000;
    PtpServo servo(8);
    for (int i = 0; i < 12; ++i) {
        servo.observe(makeExchange(1000 * i, 1000 * i + 500, O, 60000));
    }
    QVERIFY(servo.locked());
    const int64_t before = servo.offsetNs();
    QVERIFY(qAbs(before - O) <= 1);

    // One grossly delayed exchange (a 50 ms packet-delay spike in t2 only). It
    // must not unlock the servo and must not yank the smoothed offset.
    PtpExchange spike = makeExchange(99000, 99500, O, 60000);
    spike.t2 += 50'000'000; // 50 ms asymmetric jitter
    servo.observe(spike);

    QVERIFY(servo.locked());                       // still locked
    QVERIFY(qAbs(servo.offsetNs() - before) <= 1); // estimate unchanged (spike rejected)
}

void TestPtpServo::tracksSteadyOffset() {
    const int64_t O = 123456; // arbitrary steady true offset
    const int64_t D = 88000;
    PtpServo servo(8);
    for (int i = 0; i < 32; ++i) {
        servo.observe(makeExchange(2000 * i, 2000 * i + 700, O, D));
    }
    QVERIFY(servo.locked());
    QVERIFY(qAbs(servo.offsetNs() - O) <= 1);
    QVERIFY(qAbs(servo.meanPathDelayNs() - D) <= 1);
}

void TestPtpServo::tracksStepInOffset() {
    const int64_t O1 = 4000;
    const int64_t O2 = 14000; // a +10 us step in the true offset
    const int64_t D = 60000;
    const int kMin = 8;
    PtpServo servo(kMin);

    // Converge on O1.
    int t = 0;
    for (int i = 0; i < 16; ++i, ++t) {
        servo.observe(makeExchange(1000 * t, 1000 * t + 500, O1, D));
    }
    QVERIFY(servo.locked());
    QVERIFY(qAbs(servo.offsetNs() - O1) <= 1);

    // Apply a sustained step. It should ramp (NOT snap): right after the step the
    // running mean is still pulled toward O1, then converges to O2 as the window
    // fills with the new level. (A single spike would be rejected; a *run* of
    // consistent new samples is a real step and must be absorbed.)
    servo.observe(makeExchange(1000 * t, 1000 * t + 500, O2, D));
    ++t;
    const int64_t midRamp = servo.offsetNs();
    QVERIFY(midRamp > O1); // moved toward the new level
    QVERIFY(midRamp < O2); // but has not snapped all the way (ramped)

    // Feed a full window of the new level: it converges to O2.
    for (int i = 0; i < kMin; ++i, ++t) {
        servo.observe(makeExchange(1000 * t, 1000 * t + 500, O2, D));
    }
    QVERIFY(servo.locked());
    QVERIFY(qAbs(servo.offsetNs() - O2) <= 1);
}

void TestPtpServo::tracksLargeStepViaReArm() {
    // A sustained step LARGER than the outlier guard (grandmaster failover / leap /
    // coarse re-lock) must NOT lock the estimate out forever. Each post-step sample
    // is far beyond the guard; the first (window-size - 1) are held as possible
    // spikes, then the servo recognises a real step and re-arms onto the new level.
    const int64_t O1 = 4000;
    const int64_t O2 = 10'004'000; // +10 ms step, well beyond the 2 ms outlier guard
    const int64_t D = 60000;
    const int kMin = 8;
    PtpServo servo(kMin);

    int t = 0;
    for (int i = 0; i < 16; ++i, ++t) {
        servo.observe(makeExchange(1000 * t, 1000 * t + 500, O1, D));
    }
    QVERIFY(servo.locked());
    QVERIFY(qAbs(servo.offsetNs() - O1) <= 1);

    // The first kMin-1 far samples are rejected: still locked, estimate held at O1
    // (a lone spike must not jerk it). The kMin-th triggers the re-arm.
    for (int i = 0; i < kMin - 1; ++i, ++t) {
        servo.observe(makeExchange(1000 * t, 1000 * t + 500, O2, D));
        QVERIFY(servo.locked());
        QVERIFY(qAbs(servo.offsetNs() - O1) <= 1);
    }
    // Feed a full window of the new level: it re-arms and converges to O2 (NOT stuck
    // at O1 forever — the regression this pins).
    for (int i = 0; i < 2 * kMin; ++i, ++t) {
        servo.observe(makeExchange(1000 * t, 1000 * t + 500, O2, D));
    }
    QVERIFY(servo.locked());
    QVERIFY(qAbs(servo.offsetNs() - O2) <= 1);
}

void TestPtpServo::recoversLargeAbsoluteOffsetWithoutOverflow() {
    // REAL grandmaster: t1/t4 are absolute PTP ns (~1.78e18, year 2026), while
    // t2/t3 are small local-monotonic ns. The true offset (local - master) is then
    // ~-1.78e18. Summing minExchanges (8) of these in a naive running mean
    // overflows int64 (8 * 1.78e18 = 1.4e19 > 9.22e18) -> the baseline-relative
    // mean must recover the (large) offset with no signed-integer overflow (run
    // under UBSan). masterNsFromLocal(local) must also recover absolute master time.
    const int64_t masterBase = 1'780'000'000'000'000'000LL; // ~2026 in PTP ns
    const int64_t localBase = 5'000'000LL;                  // small local monotonic ns
    const int64_t D = 60'000;                               // 60 us symmetric one-way delay
    // local = master + offsetLocalMinusMaster; here local is ~1.78e18 behind master.
    const int64_t O = (localBase - masterBase); // local - master, a large negative

    PtpServo servo(8);
    for (int i = 0; i < 16; ++i) {
        const int64_t t1 = masterBase + 1'000'000LL * i;            // master Sync egress (absolute)
        const int64_t t3 = localBase + 1'000'000LL * i + 500'000LL; // local Delay_Req egress
        // t2 = local Sync ingress = t1 + D + O ; t4 = master Delay_Resp ingress = t3 + D - O.
        PtpExchange ex;
        ex.t1 = t1;
        ex.t2 = t1 + D + O; // == localBase + 1e6*i + D  (small local ns)
        ex.t3 = t3;
        ex.t4 = t3 + D - O; // == masterBase + 1e6*i + 500000 + D (absolute master ns)
        ex.valid = true;
        servo.observe(ex);
    }
    QVERIFY(servo.locked());
    // The smoothed offset recovers the large absolute offset within ns-scale tolerance.
    QVERIFY(qAbs(servo.offsetNs() - O) <= 1);
    QVERIFY(qAbs(servo.meanPathDelayNs() - D) <= 1);
    // masterNsFromLocal recovers absolute master time: master = local - offset.
    const int64_t someLocal = localBase + 123'456'789LL;
    QCOMPARE(servo.masterNsFromLocal(someLocal), someLocal - servo.offsetNs());
    QVERIFY(qAbs((someLocal - servo.offsetNs()) - (masterBase + 123'456'789LL)) <= 1);
}

QTEST_GUILESS_MAIN(TestPtpServo)
#include "tst_ptpservo.moc"
