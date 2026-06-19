// Unit tests for the pure benchmark decision core — ramp steps, sustain/headroom
// predicates, safe/ceiling feed-count derivation, and the recommendation rule.
// No threads, no codecs.
#include <QtTest>

#include "recorder_engine/benchmark/benchmarkplan.h"
#include "recorder_engine/codec/videocodecchoice.h"

class TestBenchmarkPlan : public QObject {
    Q_OBJECT
private slots:
    void rampStepsAreExact();
    void sustainPredicate();
    void headroomPredicate();
    void safeAndCeilingFromSteps();
    void recommendCodecRules(); // T-recommendation
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

// T-recommendation: pure recommendCodec() rule coverage.
void TestBenchmarkPlan::recommendCodecRules() {
    // H.264 wins when available, safeFeeds > 0, and >= mpeg2
    QCOMPARE(recommendCodec(true, 8, 4), VideoCodecChoice::H264Hardware);

    // Tie (equal, both > 0) -> H.264
    QCOMPARE(recommendCodec(true, 4, 4), VideoCodecChoice::H264Hardware);

    // h264SafeFeeds == 0 -> MPEG-2 even if mpeg2 also 0
    QCOMPARE(recommendCodec(true, 0, 0), VideoCodecChoice::Mpeg2Software);

    // h264Available == false -> MPEG-2 regardless
    QCOMPARE(recommendCodec(false, 8, 4), VideoCodecChoice::Mpeg2Software);

    // mpeg2 higher -> MPEG-2
    QCOMPARE(recommendCodec(true, 4, 8), VideoCodecChoice::Mpeg2Software);
}

QTEST_GUILESS_MAIN(TestBenchmarkPlan)
#include "tst_benchmarkplan.moc"
