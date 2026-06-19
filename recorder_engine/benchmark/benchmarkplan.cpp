#include "recorder_engine/benchmark/benchmarkplan.h"

QVector<int> benchmarkRampSteps() {
    return {1, 2, 4, 8, 12, 16, 20, 24, 28, 32};
}

bool rampStepSustained(const RampStepResult& r) {
    return r.budgetMet && r.framesProcessed >= 0.95 * r.framesRequired;
}

bool rampStepHasHeadroom(const RampStepResult& r) {
    return r.budgetMet && r.framesProcessed >= 1.2 * r.framesRequired;
}

int ceilingFeedCount(const QVector<RampStepResult>& steps) {
    int best = 0;
    for (const RampStepResult& r : steps)
        if (rampStepSustained(r) && r.concurrency > best) best = r.concurrency;
    return best;
}

int safeFeedCount(const QVector<RampStepResult>& steps) {
    int best = 0;
    for (const RampStepResult& r : steps)
        if (rampStepHasHeadroom(r) && r.concurrency > best) best = r.concurrency;
    return best;
}
