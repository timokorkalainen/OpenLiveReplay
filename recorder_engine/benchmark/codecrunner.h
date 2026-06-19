#ifndef OLR_CODECRUNNER_H
#define OLR_CODECRUNNER_H

#include "recorder_engine/benchmark/benchmarktypes.h"

// Runs N concurrent encode+decode pipelines for a measurement window and reports
// the aggregate. Implementations: real (FFmpeg MPEG-2, native H.264) and fake (tests).
class CodecRunner {
public:
    virtual ~CodecRunner() = default;
    virtual RampStepResult runStep(int concurrency, const BenchmarkConfig& config) = 0;
    virtual bool available() const = 0;
};

#endif // OLR_CODECRUNNER_H
