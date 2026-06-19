#include <QtTest>
#include "recorder_engine/timing/sourceoffsetestimator.h"

class TestSourceOffsetEstimator : public QObject {
    Q_OBJECT
private slots:
    void noEvidenceDefaultsApproximate();
    void timecodeAlignedIsFrameAccurate();
    void externalReferenceIsFrameAccurate();
    void lockedClockIsBoundedWithMsBound();
    void unlockedArrivalIsApproximate();
    void offsetIsRecorded();
    void resetClears();
    void externalReferenceWithoutTcOrLockIsFrameAccurate();
    void flvPllBoundIsWiderThanPcr();
    void ppmTermWidensBoundedBound();
    void outOfRangeAccessorsAreSafe();
};

void TestSourceOffsetEstimator::noEvidenceDefaultsApproximate() {
    SourceOffsetEstimator e;
    QVERIFY(!e.hasEvidence(0));
    QCOMPARE(e.tier(0), ConfidenceTier::Approximate);
}
void TestSourceOffsetEstimator::timecodeAlignedIsFrameAccurate() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Pcr;
    ev.clockLocked = true;
    ev.timecodeAlignedToReference = true;
    ev.measuredOffsetMs = 0;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::FrameAccurate);
    QCOMPARE(e.boundMs(0), 0);
}
void TestSourceOffsetEstimator::externalReferenceIsFrameAccurate() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.externalReference = true;
    ev.clockLocked = true;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::FrameAccurate);
}
void TestSourceOffsetEstimator::lockedClockIsBoundedWithMsBound() {
    SourceOffsetEstimator e(/*boundedBaseMs*/ 4);
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Pcr;
    ev.clockLocked = true;
    ev.timecodeAlignedToReference = false;
    ev.measuredOffsetMs = 7;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::Bounded);
    QVERIFY(e.boundMs(0) >= 4); // at least the base bound
    QCOMPARE(e.offsetMs(0), int64_t(7));
}
void TestSourceOffsetEstimator::unlockedArrivalIsApproximate() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Arrival;
    ev.clockLocked = false;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::Approximate);
    QCOMPARE(e.boundMs(0), 40);
}
void TestSourceOffsetEstimator::offsetIsRecorded() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockLocked = true;
    ev.clockQuality = ClockQuality::Ndi;
    ev.measuredOffsetMs = -12;
    e.update(1, ev);
    QCOMPARE(e.offsetMs(1), int64_t(-12));
}
void TestSourceOffsetEstimator::resetClears() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockLocked = true;
    e.update(0, ev);
    e.reset();
    QVERIFY(!e.hasEvidence(0));
}
void TestSourceOffsetEstimator::externalReferenceWithoutTcOrLockIsFrameAccurate() {
    // An external reference (e.g. PTP) is frame-accurate even with no common TC
    // and an unlocked source clock — the reference itself supplies the timebase.
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockQuality = ClockQuality::Arrival;
    ev.clockLocked = false;
    ev.timecodeAlignedToReference = false;
    ev.externalReference = true;
    e.update(0, ev);
    QCOMPARE(e.tier(0), ConfidenceTier::FrameAccurate);
    QCOMPARE(e.boundMs(0), 0);
}
void TestSourceOffsetEstimator::flvPllBoundIsWiderThanPcr() {
    // The weaker FLV/RTMP clock must carry a wider Bounded ±ms than PCR/NDI.
    SourceOffsetEstimator e(/*boundedBaseMs*/ 4);
    SourcePhaseEvidence pcr;
    pcr.clockQuality = ClockQuality::Pcr;
    pcr.clockLocked = true;
    e.update(0, pcr);
    SourcePhaseEvidence flv;
    flv.clockQuality = ClockQuality::FlvPll;
    flv.clockLocked = true;
    e.update(1, flv);
    QCOMPARE(e.tier(0), ConfidenceTier::Bounded);
    QCOMPARE(e.tier(1), ConfidenceTier::Bounded);
    QVERIFY(e.boundMs(1) > e.boundMs(0));
}
void TestSourceOffsetEstimator::ppmTermWidensBoundedBound() {
    // With a non-zero residual-drift coefficient, a larger |ppm| widens the bound.
    SourceOffsetEstimator e(/*boundedBaseMs*/ 4, /*boundedPpmMsPerSec*/ 1000);
    SourcePhaseEvidence calm;
    calm.clockQuality = ClockQuality::Pcr;
    calm.clockLocked = true;
    calm.clockPpm = 0.0;
    e.update(0, calm);
    SourcePhaseEvidence skewed;
    skewed.clockQuality = ClockQuality::Pcr;
    skewed.clockLocked = true;
    skewed.clockPpm = 5000.0; // large residual ppm
    e.update(1, skewed);
    QVERIFY(e.boundMs(1) > e.boundMs(0));
}
void TestSourceOffsetEstimator::outOfRangeAccessorsAreSafe() {
    SourceOffsetEstimator e;
    SourcePhaseEvidence ev;
    ev.clockLocked = true;
    e.update(-1, ev);  // ignored, no OOB
    e.update(999, ev); // ignored, no OOB
    QCOMPARE(e.tier(-1), ConfidenceTier::Approximate);
    QCOMPARE(e.tier(999), ConfidenceTier::Approximate);
    QCOMPARE(e.boundMs(-1), 40);
    QCOMPARE(e.offsetMs(999), int64_t(0));
    QVERIFY(!e.hasEvidence(-1));
}
QTEST_GUILESS_MAIN(TestSourceOffsetEstimator)
#include "tst_sourceoffsetestimator.moc"
