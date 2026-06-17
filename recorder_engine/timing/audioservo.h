#ifndef AUDIOSERVO_H
#define AUDIOSERVO_H

#include <cstdint>

double clampPpm(double ppm, double maxPpm);
int64_t correctedSrcSamples(int64_t nominalSamples, double ppm);
int64_t correctedSrcSamplesAccumulated(int64_t nominalSamples, double ppm,
                                       double* fractionalSamples);

#endif // AUDIOSERVO_H
