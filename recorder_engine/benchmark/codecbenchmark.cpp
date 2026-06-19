#include "recorder_engine/benchmark/codecbenchmark.h"

CodecBenchmark::CodecResult CodecBenchmark::rampCodec(
    CodecRunner& runner, const BenchmarkConfig& config,
    const ProgressFn& onStep, const std::atomic<bool>& cancel) {
    CodecResult out;
    if (!runner.available()) return out;

    const QVector<int> steps = benchmarkRampSteps();
    for (int i = 0; i < steps.size(); ++i) {
        if (cancel.load(std::memory_order_acquire)) break;
        const int n = steps[i];
        RampStepResult r = runner.runStep(n, config);
        out.steps.append(r);
        const bool sustained = rampStepSustained(r);
        if (onStep) onStep(n, sustained);
        if (!sustained) break;                  // stop at first failing step
        if (n == steps.last()) out.ceilingReached = true; // reached 32 still sustaining
    }
    out.safeFeeds = safeFeedCount(out.steps);
    out.ceiling = ceilingFeedCount(out.steps);
    // Report avg ms from the largest sustained step (most representative of real load).
    for (const RampStepResult& r : out.steps)
        if (rampStepSustained(r) && r.concurrency == out.ceiling) {
            out.encodeMs = r.avgEncodeMs; out.decodeMs = r.avgDecodeMs;
        }
    return out;
}
