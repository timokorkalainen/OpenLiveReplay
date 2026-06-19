#ifndef OLR_RUNCODECBENCHMARK_H
#define OLR_RUNCODECBENCHMARK_H

#include "recorder_engine/benchmark/benchmarktypes.h"
#include "recorder_engine/benchmark/codecbenchmark.h"

#include <atomic>

// Top-level blocking benchmark entry point. Meant to be called on a worker thread.
// Constructs real runners, runs rampCodec for each codec, and assembles a full
// CodecBenchmarkResult. The caller is responsible for stamping result.timestamp.
CodecBenchmarkResult runCodecBenchmark(const BenchmarkConfig& config,
                                       const CodecBenchmark::ProgressFn& onStep,
                                       const std::atomic<bool>& cancel);

#endif // OLR_RUNCODECBENCHMARK_H
