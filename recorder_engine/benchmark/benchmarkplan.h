#ifndef OLR_BENCHMARKPLAN_H
#define OLR_BENCHMARKPLAN_H

#include "recorder_engine/benchmark/benchmarktypes.h"

#include <QVector>

// The concurrency ramp, coarse early then fine through the high end.
QVector<int> benchmarkRampSteps();

// Step N keeps up with real time (>=95% of required pairs, no frame over budget).
bool rampStepSustained(const RampStepResult& r);
// Step N has comfortable margin (>=120% of required pairs, no frame over budget).
bool rampStepHasHeadroom(const RampStepResult& r);

// Largest N that sustained; 0 if none.
int ceilingFeedCount(const QVector<RampStepResult>& steps);
// Largest N that had headroom (the recommended safe feed count); 0 if none.
int safeFeedCount(const QVector<RampStepResult>& steps);

#endif // OLR_BENCHMARKPLAN_H
