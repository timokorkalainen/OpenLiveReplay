#include "timecodeevidence.h"

#include <cmath>
#include <limits>

namespace {

constexpr int64_t kMinSupportedFps = 12;
constexpr int64_t kMaxSupportedFps = 240;
constexpr int64_t kMaxRateDenominator = 100000;

bool checkedMultiplyAdd(int64_t a, int64_t b, int64_t c, int64_t* result) {
    if (a < 0 || b < 0 || c < 0) return false;
    if (a != 0 && b > (std::numeric_limits<int64_t>::max() - c) / a) return false;
    *result = a * b + c;
    return true;
}

bool supportedRate(FrameRateQ rate) {
    if (!rate.valid()) return false;
    const int64_t numerator = rate.num;
    const int64_t denominator = rate.den;
    int64_t minimum = 0;
    int64_t maximum = 0;
    return checkedMultiplyAdd(kMinSupportedFps, denominator, 0, &minimum) &&
           checkedMultiplyAdd(kMaxSupportedFps, denominator, 0, &maximum) && numerator >= minimum &&
           numerator <= maximum;
}

std::optional<FrameRateQ> boundedContinuedFraction(double value) {
    int64_t p0 = 0;
    int64_t q0 = 1;
    int64_t p1 = 1;
    int64_t q1 = 0;
    double remainder = value;

    while (true) {
        const double coefficientValue = std::floor(remainder);
        if (!std::isfinite(coefficientValue) || coefficientValue < 0.0 ||
            coefficientValue >= double(std::numeric_limits<int64_t>::max()))
            return std::nullopt;
        const int64_t coefficient = static_cast<int64_t>(coefficientValue);

        int64_t q2 = 0;
        const bool denominatorExceedsCap = q1 > 0 && coefficient > (kMaxRateDenominator - q0) / q1;
        if (!denominatorExceedsCap && !checkedMultiplyAdd(coefficient, q1, q0, &q2))
            return std::nullopt;
        if (denominatorExceedsCap || q2 > kMaxRateDenominator) {
            if (q1 <= 0) return std::nullopt;
            const int64_t scale = (kMaxRateDenominator - q0) / q1;
            int64_t boundedNumerator = 0;
            int64_t boundedDenominator = 0;
            if (!checkedMultiplyAdd(scale, p1, p0, &boundedNumerator) ||
                !checkedMultiplyAdd(scale, q1, q0, &boundedDenominator))
                return std::nullopt;

            const double convergentError = std::abs(double(p1) / double(q1) - value);
            const double boundedError =
                std::abs(double(boundedNumerator) / double(boundedDenominator) - value);
            if (boundedError < convergentError) {
                p1 = boundedNumerator;
                q1 = boundedDenominator;
            }
            break;
        }

        int64_t p2 = 0;
        if (!checkedMultiplyAdd(coefficient, p1, p0, &p2)) return std::nullopt;
        p0 = p1;
        q0 = q1;
        p1 = p2;
        q1 = q2;

        const double fraction = remainder - coefficientValue;
        if (fraction == 0.0) break;
        remainder = 1.0 / fraction;
        if (!std::isfinite(remainder)) break;
    }

    if (p1 <= 0 || p1 > std::numeric_limits<int32_t>::max() || q1 <= 0 ||
        q1 > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    return FrameRateQ{int32_t(p1), int32_t(q1)};
}

} // namespace

bool TimecodeEvidence::valid() const {
    if (frameOfDay < 0 || arrivalSessionFrame < 0 || quantizationBoundUs < 0 || driftBoundUs < 0)
        return false;
    if (!supportedRate(labelRate) || !supportedRate(sessionRate)) return false;
    if (quantizationBoundUs > std::numeric_limits<int64_t>::max() - driftBoundUs) return false;

    const int nominalRate = Smpte12m::labelRate(labelRate.num, labelRate.den);
    int64_t framesPerDay = 0;
    if (nominalRate <= 0 || !checkedMultiplyAdd(nominalRate, 24 * 60 * 60, 0, &framesPerDay) ||
        frameOfDay >= framesPerDay)
        return false;

    if (dropFrame) {
        const FrameRateQ ntsc30{30000, 1001};
        const FrameRateQ ntsc60{60000, 1001};
        if (!(labelRate == ntsc30) && !(labelRate == ntsc60)) return false;
    }
    return true;
}

std::optional<FrameRateQ> canonicalFrameRate(double framesPerSecond) {
    if (!std::isfinite(framesPerSecond) || framesPerSecond < double(kMinSupportedFps) ||
        framesPerSecond > double(kMaxSupportedFps))
        return std::nullopt;

    struct CanonicalRate {
        double fps;
        FrameRateQ rate;
    };
    constexpr CanonicalRate canonical[] = {
        {25.0, {25, 1}}, {30000.0 / 1001.0, {30000, 1001}}, {30.0, {30, 1}},
        {50.0, {50, 1}}, {60000.0 / 1001.0, {60000, 1001}}, {60.0, {60, 1}},
    };
    constexpr double kCanonicalToleranceFps = 0.001;
    for (const CanonicalRate& candidate : canonical) {
        if (std::abs(framesPerSecond - candidate.fps) <= kCanonicalToleranceFps)
            return candidate.rate;
    }

    return boundedContinuedFraction(framesPerSecond);
}
