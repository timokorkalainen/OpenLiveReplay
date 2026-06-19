// Unit tests for the pure benchmark decision core — ramp steps, sustain/headroom
// predicates, and safe/ceiling feed-count derivation. No threads, no codecs.
#include <QtTest>

#include "recorder_engine/benchmark/benchmarkplan.h"

class TestBenchmarkPlan : public QObject {
    Q_OBJECT
private slots:
    void rampStepsAreExact();
    void sustainPredicate();
    void headroomPredicate();
    void safeAndCeilingFromSteps();
};

void TestBenchmarkPlan::rampStepsAreExact() {
    QCOMPARE(benchmarkRampSteps(), (QVector<int>{1, 2, 4, 8, 12, 16, 20, 24, 28, 32}));
}

void TestBenchmarkPlan::sustainPredicate() {
    RampStepResult r; r.concurrency = 4; r.framesRequired = 600;
    r.framesProcessed = 600; r.budgetMet = true;
    QVERIFY(rampStepSustained(r));
    r.framesProcessed = 570; QVERIFY(rampStepSustained(r));   // exactly 0.95
    r.framesProcessed = 560; QVERIFY(!rampStepSustained(r));  // below 0.95
    r.framesProcessed = 600; r.budgetMet = false; QVERIFY(!rampStepSustained(r));
}

void TestBenchmarkPlan::headroomPredicate() {
    RampStepResult r; r.framesRequired = 600; r.budgetMet = true;
    r.framesProcessed = 720; QVERIFY(rampStepHasHeadroom(r));   // exactly 1.2
    r.framesProcessed = 719; QVERIFY(!rampStepHasHeadroom(r));
}

void TestBenchmarkPlan::safeAndCeilingFromSteps() {
    // N=1,2,4 strong; N=8 sustained but tight (no headroom); N=12 fails.
    auto mk = [](int n, int req, int got) {
        RampStepResult r; r.concurrency = n; r.framesRequired = req;
        r.framesProcessed = got; r.budgetMet = true; return r;
    };
    QVector<RampStepResult> steps{
        mk(1, 150, 300), mk(2, 300, 600), mk(4, 600, 1200),
        mk(8, 1200, 1260),   // sustained (>=0.95*1200=1140) but < headroom (1440)
        mk(12, 1800, 1500),  // not sustained (< 1710)
    };
    QCOMPARE(ceilingFeedCount(steps), 8); // largest sustained
    QCOMPARE(safeFeedCount(steps), 4);    // largest with 1.2x headroom
}

QTEST_GUILESS_MAIN(TestBenchmarkPlan)
#include "tst_benchmarkplan.moc"
