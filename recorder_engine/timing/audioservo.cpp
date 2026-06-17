#include "audioservo.h"

#include <algorithm>
#include <cmath>

double clampPpm(double ppm, double maxPpm) {
    const double limit = std::abs(maxPpm);
    return std::max(-limit, std::min(limit, ppm));
}

int64_t correctedSrcSamples(int64_t nominalSamples, double ppm) {
    if (nominalSamples <= 0) {
        return 0;
    }
    return std::max<int64_t>(
        0, int64_t(std::llround(double(nominalSamples) * (1.0 + ppm / 1000000.0))));
}

int64_t correctedSrcSamplesAccumulated(int64_t nominalSamples, double ppm,
                                       double* fractionalSamples) {
    if (nominalSamples <= 0) {
        return 0;
    }
    double carry = fractionalSamples ? *fractionalSamples : 0.0;
    const double exact = double(nominalSamples) * (1.0 + ppm / 1000000.0) + carry;
    const int64_t corrected = std::max<int64_t>(0, int64_t(std::llround(exact)));
    if (fractionalSamples) {
        *fractionalSamples = exact - double(corrected);
    }
    return corrected;
}
