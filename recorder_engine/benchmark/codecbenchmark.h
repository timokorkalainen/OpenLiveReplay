#ifndef OLR_CODECBENCHMARK_H
#define OLR_CODECBENCHMARK_H

#include "recorder_engine/benchmark/benchmarkplan.h"
#include "recorder_engine/benchmark/codecrunner.h"

#include <atomic>
#include <functional>

class CodecBenchmark {
public:
    using ProgressFn = std::function<void(int concurrency, bool sustained)>;
    struct CodecResult {
        int safeFeeds = 0;
        int ceiling = 0;
        double encodeMs = 0.0;
        double decodeMs = 0.0;
        bool ceilingReached = false;
        QVector<RampStepResult> steps;
    };
    // Walk the ramp on the CALLING thread (the caller is already a worker thread).
    // Stops at the first non-sustained step, on cancel, or after the last ramp step.
    static CodecResult rampCodec(CodecRunner& runner, const BenchmarkConfig& config,
                                 const ProgressFn& onStep, const std::atomic<bool>& cancel);
};

#endif // OLR_CODECBENCHMARK_H
