#include "timecodealignerv2.h"

#include <limits>

namespace {
using I128 = __int128;

// Microseconds(frames / rate) with a single explicit round-to-nearest. The full
// int64 frame and int32 rate domains fit in __int128; callers check before
// narrowing a final result to int64.
I128 usFor(int64_t frames, FrameRateQ rate) {
    const I128 numerator = I128(frames) * 1'000'000 * rate.den;
    const I128 denominator = I128(rate.num);
    return (numerator >= 0 ? numerator + denominator / 2 : numerator - denominator / 2) /
           denominator;
}

// Ceil of one frame period in microseconds (worst-case arrival quantization).
I128 framePeriodUs(FrameRateQ rate) {
    return (I128(1'000'000) * rate.den + rate.num - 1) / rate.num;
}

bool fitsInt64(I128 value) {
    return value >= I128(std::numeric_limits<int64_t>::min()) &&
           value <= I128(std::numeric_limits<int64_t>::max());
}

bool supportedRate(FrameRateQ rate) {
    if (!rate.valid()) return false;
    const I128 numerator = rate.num;
    const I128 denominator = rate.den;
    return numerator >= I128(12) * denominator && numerator <= I128(240) * denominator;
}
} // namespace

void TimecodeAlignerV2::observe(int source, int64_t tcFrames, FrameRateQ tcRate,
                                int64_t sessionFrame, FrameRateQ sessionRate) {
    if (source < 0 || source >= kMaxSources || tcFrames < 0 || sessionFrame < 0) return;
    if (!supportedRate(tcRate) || !supportedRate(sessionRate)) return;
    Anchor& anchor = m_anchors[source];
    if (anchor.set) return;
    anchor = Anchor{true, tcFrames, tcRate, sessionFrame, sessionRate};
}

bool TimecodeAlignerV2::hasTimecode(int source) const {
    return source >= 0 && source < kMaxSources && m_anchors[source].set;
}

AlignmentOffset TimecodeAlignerV2::offset(int sourceA, int sourceB, int32_t driftPpm) const {
    AlignmentOffset out;
    if (!hasTimecode(sourceA) || !hasTimecode(sourceB)) return out;
    const Anchor& anchorA = m_anchors[sourceA];
    const Anchor& anchorB = m_anchors[sourceB];

    // skew_i = sessionFrame/sessionRate - tcFrames/tcRate, in microseconds.
    const I128 sessionA = usFor(anchorA.sessionFrame, anchorA.sessionRate);
    const I128 sessionB = usFor(anchorB.sessionFrame, anchorB.sessionRate);
    const I128 timecodeA = usFor(anchorA.tcFrames, anchorA.tcRate);
    const I128 timecodeB = usFor(anchorB.tcFrames, anchorB.tcRate);
    if (!fitsInt64(sessionA) || !fitsInt64(sessionB) || !fitsInt64(timecodeA) ||
        !fitsInt64(timecodeB))
        return out;
    const I128 skewA = sessionA - timecodeA;
    const I128 skewB = sessionB - timecodeB;
    if (!fitsInt64(skewA) || !fitsInt64(skewB)) return out;
    const I128 offsetUs = skewA - skewB;
    if (!fitsInt64(offsetUs)) return out;

    // Bound = one session frame (arrival quantization, worst of the two rates)
    // plus drift residual (absolute anchor-TC skew times absolute ppm).
    const I128 framePeriodA = framePeriodUs(anchorA.sessionRate);
    const I128 framePeriodB = framePeriodUs(anchorB.sessionRate);
    const I128 quantizationBoundUs = framePeriodA > framePeriodB ? framePeriodA : framePeriodB;
    const I128 timecodeSkewUs = timecodeA - timecodeB;
    if (!fitsInt64(quantizationBoundUs) || !fitsInt64(timecodeSkewUs)) return out;
    const I128 absoluteTimecodeSkewUs = timecodeSkewUs < 0 ? -timecodeSkewUs : timecodeSkewUs;
    const I128 driftPpmMagnitude = driftPpm < 0 ? -I128(driftPpm) : I128(driftPpm);

    I128 driftBoundUs = 0;
    if (driftPpmMagnitude > 0) {
        const I128 maximumSafeSkew =
            (I128(std::numeric_limits<int64_t>::max()) * 1'000'000) / driftPpmMagnitude;
        if (absoluteTimecodeSkewUs > maximumSafeSkew) return out;
        driftBoundUs = (absoluteTimecodeSkewUs * driftPpmMagnitude) / 1'000'000;
    }

    const I128 boundUs = quantizationBoundUs + driftBoundUs;
    if (!fitsInt64(boundUs)) return out;

    out.offsetUs = int64_t(offsetUs);
    out.boundUs = int64_t(boundUs);
    out.kind = out.boundUs == 0 ? AlignmentOffset::Kind::Exact : AlignmentOffset::Kind::Bounded;
    return out;
}

void TimecodeAlignerV2::reset() {
    for (auto& anchor : m_anchors)
        anchor = Anchor{};
}
