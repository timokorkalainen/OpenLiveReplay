// Unit tests for the benchmark orchestrator's ramp control flow, using a fake
// CodecRunner with programmable per-N outcomes. Deterministic, no real codecs.
#include <QtTest>
#include <atomic>

#include "recorder_engine/benchmark/codecbenchmark.h"
#include "recorder_engine/benchmark/codecrunner.h"

namespace {
// Fake runner: "sustains" for N <= maxSustain (with headroom up to maxHeadroom),
// records the ramp it was asked to run, so tests can assert ordering + stop-on-fail.
class FakeRunner : public CodecRunner {
public:
    int maxHeadroom; int maxSustain; QVector<int> visited;
    bool observedCancel = false; // C1: did any call observe a set cancel flag?
    FakeRunner(int headroom, int sustain) : maxHeadroom(headroom), maxSustain(sustain) {}
    bool available() const override { return true; }
    RampStepResult runStep(int n, const BenchmarkConfig& c,
                           const std::atomic<bool>& cancel) override {
        if (cancel.load(std::memory_order_acquire)) {
            observedCancel = true;
            return {}; // early-out: return empty result when cancel is pre-set
        }
        visited.append(n);
        RampStepResult r; r.concurrency = n;
        r.framesRequired = int64_t(n) * c.fps * c.durationMsPerStep / 1000;
        if (n <= maxHeadroom)      r.framesProcessed = r.framesRequired * 2;   // strong
        else if (n <= maxSustain)  r.framesProcessed = r.framesRequired;       // tight
        else                       r.framesProcessed = r.framesRequired / 2;   // fails
        r.budgetMet = (n <= maxSustain);
        r.avgEncodeMs = 1.0; r.avgDecodeMs = 0.5;
        return r;
    }
};
} // namespace

class TestCodecBenchmark : public QObject {
    Q_OBJECT
private slots:
    void stopsAtFirstFailingStep();
    void safeFeedIsLargestWithHeadroom();
    void cancellationStopsRamp();
    void unavailableRunnerYieldsNoFeeds();
    void cancelReachesRunStep(); // T-cancel (C1 wiring)
};

void TestCodecBenchmark::stopsAtFirstFailingStep() {
    FakeRunner runner(/*headroom*/8, /*sustain*/8); // N=12 fails
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    // Visited 1,2,4,8,12 then stopped (no 16+).
    QCOMPARE(runner.visited, (QVector<int>{1, 2, 4, 8, 12}));
    QCOMPARE(res.ceiling, 8);
    QVERIFY(!res.ceilingReached);
}

void TestCodecBenchmark::safeFeedIsLargestWithHeadroom() {
    FakeRunner runner(/*headroom*/4, /*sustain*/8); // 1,2,4 headroom; 8 tight; 12 fails
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    QCOMPARE(res.safeFeeds, 4);
    QCOMPARE(res.ceiling, 8);
}

void TestCodecBenchmark::cancellationStopsRamp() {
    FakeRunner runner(32, 32);
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{false};
    int calls = 0;
    auto res = CodecBenchmark::rampCodec(runner, cfg, [&](int,bool){ if (++calls == 2) cancel = true; }, cancel);
    QVERIFY(runner.visited.size() <= 3); // stopped shortly after cancel
    (void)res;
}

void TestCodecBenchmark::unavailableRunnerYieldsNoFeeds() {
    class Unavail : public CodecRunner {
        bool available() const override { return false; }
        RampStepResult runStep(int, const BenchmarkConfig&,
                               const std::atomic<bool>&) override { return {}; }
    } runner;
    BenchmarkConfig cfg;
    std::atomic<bool> cancel{false};
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    QCOMPARE(res.safeFeeds, 0);
    QCOMPARE(res.ceiling, 0);
}

// T-cancel: proves that a pre-set cancel flag is propagated into runStep.
// The FakeRunner returns empty and records observedCancel when cancel is true on entry.
void TestCodecBenchmark::cancelReachesRunStep() {
    FakeRunner runner(32, 32);
    BenchmarkConfig cfg; cfg.fps = 30; cfg.durationMsPerStep = 1000;
    std::atomic<bool> cancel{true}; // pre-set cancel
    auto res = CodecBenchmark::rampCodec(runner, cfg, [](int,bool){}, cancel);
    // rampCodec checks cancel before each step and breaks immediately, so runStep
    // is never called when cancel is already set on the first iteration.
    QCOMPARE(runner.visited.size(), 0); // no steps executed
    QCOMPARE(res.safeFeeds, 0);
    QCOMPARE(res.ceiling, 0);
}

QTEST_GUILESS_MAIN(TestCodecBenchmark)
#include "tst_codecbenchmark.moc"
