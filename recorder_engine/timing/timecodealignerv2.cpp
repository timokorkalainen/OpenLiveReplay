#include "timecodealignerv2.h"

#include <limits>

namespace {
using I128 = __int128;

struct Rational {
    I128 numerator = 0;
    I128 denominator = 1;
};

bool checkedAdd(I128 a, I128 b, I128* out) {
    return !__builtin_add_overflow(a, b, out);
}

bool checkedSubtract(I128 a, I128 b, I128* out) {
    return !__builtin_sub_overflow(a, b, out);
}

bool checkedMultiply(I128 a, I128 b, I128* out) {
    return !__builtin_mul_overflow(a, b, out);
}

I128 absolute(I128 value) {
    return value < 0 ? -value : value;
}

I128 greatestCommonDivisor(I128 a, I128 b) {
    a = absolute(a);
    b = absolute(b);
    while (b != 0) {
        const I128 remainder = a % b;
        a = b;
        b = remainder;
    }
    return a == 0 ? 1 : a;
}

void reduce(Rational* value) {
    const I128 divisor = greatestCommonDivisor(value->numerator, value->denominator);
    value->numerator /= divisor;
    value->denominator /= divisor;
}

bool timeUs(int64_t frames, FrameRateQ rate, Rational* out) {
    if (!rate.valid()) return false;
    I128 numerator = 0;
    if (!checkedMultiply(I128(frames), I128(rate.den), &numerator) ||
        !checkedMultiply(numerator, I128(1'000'000), &numerator))
        return false;
    *out = Rational{numerator, I128(rate.num)};
    reduce(out);
    return true;
}

bool subtract(Rational a, Rational b, Rational* out) {
    const I128 divisor = greatestCommonDivisor(a.denominator, b.denominator);
    const I128 leftMultiplier = b.denominator / divisor;
    const I128 rightMultiplier = a.denominator / divisor;
    I128 left = 0;
    I128 right = 0;
    I128 numerator = 0;
    I128 denominator = 0;
    if (!checkedMultiply(a.numerator, leftMultiplier, &left) ||
        !checkedMultiply(b.numerator, rightMultiplier, &right) ||
        !checkedSubtract(left, right, &numerator) ||
        !checkedMultiply(a.denominator, leftMultiplier, &denominator))
        return false;
    *out = Rational{numerator, denominator};
    reduce(out);
    return true;
}

bool compareAbsolute(Rational a, Rational b, int* result) {
    I128 left = 0;
    I128 right = 0;
    if (!checkedMultiply(absolute(a.numerator), b.denominator, &left) ||
        !checkedMultiply(absolute(b.numerator), a.denominator, &right))
        return false;
    *result = left < right ? -1 : (left > right ? 1 : 0);
    return true;
}

bool absoluteWithinUs(Rational value, int64_t boundUs, bool strict = false) {
    if (boundUs < 0) return false;
    I128 right = 0;
    if (!checkedMultiply(I128(boundUs), value.denominator, &right)) return false;
    return strict ? absolute(value.numerator) < right : absolute(value.numerator) <= right;
}

bool roundToInt64(Rational value, int64_t* out) {
    I128 roundedNumerator = 0;
    if (!checkedAdd(absolute(value.numerator), value.denominator / 2, &roundedNumerator))
        return false;
    I128 rounded = roundedNumerator / value.denominator;
    if (value.numerator < 0) rounded = -rounded;
    if (rounded < I128(std::numeric_limits<int64_t>::min()) ||
        rounded > I128(std::numeric_limits<int64_t>::max()))
        return false;
    *out = int64_t(rounded);
    return true;
}

bool ceilScaledAbsolute(Rational value, I128 scaleNumerator, I128 scaleDenominator, I128* out) {
    I128 numerator = 0;
    I128 denominator = 0;
    I128 adjusted = 0;
    if (!checkedMultiply(absolute(value.numerator), scaleNumerator, &numerator) ||
        !checkedMultiply(value.denominator, scaleDenominator, &denominator) ||
        !checkedAdd(numerator, denominator - 1, &adjusted))
        return false;
    *out = adjusted / denominator;
    return true;
}

bool framesPerDay(const TimecodeEvidence& evidence, int64_t* out) {
    const int nominalRate = Smpte12m::labelRate(evidence.labelRate.num, evidence.labelRate.den);
    if (nominalRate <= 0) return false;

    I128 frames = 0;
    if (evidence.provenance == TimecodeProvenance::Ndi) {
        frames = (I128(evidence.labelRate.num) * (24 * 60 * 60) + evidence.labelRate.den / 2) /
                 evidence.labelRate.den;
        if (evidence.dropFrame) return false;
    } else {
        frames = I128(nominalRate) * (24 * 60 * 60);
        if (evidence.dropFrame) {
            int droppedLabelsPerMinute = 0;
            if (evidence.labelRate == FrameRateQ{30000, 1001})
                droppedLabelsPerMinute = 2;
            else if (evidence.labelRate == FrameRateQ{60000, 1001})
                droppedLabelsPerMinute = 4;
            else
                return false;
            constexpr int kDropMinutesPerDay = 24 * 60 - (24 * 60) / 10;
            frames -= I128(droppedLabelsPerMinute) * kDropMinutesPerDay;
        }
    }
    if (frames <= 0 || frames > I128(std::numeric_limits<int64_t>::max())) return false;
    *out = int64_t(frames);
    return true;
}

bool matchingClock(const TimecodeEvidence& a, const TimecodeEvidence& b) {
    return a.labelRate == b.labelRate && a.sessionRate == b.sessionRate &&
           a.dropFrame == b.dropFrame && a.provenance == b.provenance;
}

bool combinedObservationBound(const TimecodeEvidence& a, const TimecodeEvidence& b, int64_t* out) {
    I128 sum =
        I128(a.quantizationBoundUs) + a.driftBoundUs + b.quantizationBoundUs + b.driftBoundUs;
    if (sum < 0 || sum > I128(std::numeric_limits<int64_t>::max())) return false;
    *out = int64_t(sum);
    return true;
}
} // namespace

void TimecodeAlignerV2::observe(int source, int64_t tcFrames, FrameRateQ tcRate,
                                int64_t sessionFrame, FrameRateQ sessionRate) {
    if (!sessionRate.valid()) return;
    const I128 frameBound =
        (I128(1'000'000) * sessionRate.den + sessionRate.num - 1) / sessionRate.num;
    if (frameBound > I128(std::numeric_limits<int64_t>::max())) return;

    TimecodeEvidence evidence;
    evidence.frameOfDay = tcFrames;
    evidence.labelRate = tcRate;
    evidence.provenance = TimecodeProvenance::Ndi;
    evidence.arrivalSessionFrame = sessionFrame;
    evidence.sessionRate = sessionRate;
    evidence.quantizationBoundUs = int64_t(frameBound);
    observe(source, evidence);
}

void TimecodeAlignerV2::observe(int source, const TimecodeEvidence& evidence) {
    if (source < 0 || source >= kMaxSources) return;
    SourceState& state = m_sources[source];

    if (state.generationSet) {
        if (evidence.sourceGeneration < state.sourceGeneration ||
            (evidence.sourceGeneration == state.sourceGeneration &&
             evidence.timingGeneration < state.timingGeneration))
            return;

        const bool newerSource = evidence.sourceGeneration > state.sourceGeneration;
        const bool newerTiming = evidence.sourceGeneration == state.sourceGeneration &&
                                 evidence.timingGeneration > state.timingGeneration;
        if (newerSource || newerTiming) state = SourceState{};
    }

    if (!state.generationSet) {
        state.generationSet = true;
        state.sourceGeneration = evidence.sourceGeneration;
        state.timingGeneration = evidence.timingGeneration;
    }

    if (state.invalid) return;
    if (!evidence.valid() || evidence.discontinuity) {
        state.invalid = true;
        return;
    }

    if (!state.anchorSet) {
        const Observation observation{evidence, evidence.frameOfDay};
        state.anchor = observation;
        state.previous = observation;
        state.anchorSet = true;
        return;
    }

    if (!matchingClock(state.previous.evidence, evidence)) {
        state.invalid = true;
        return;
    }
    if (evidence.arrivalSessionFrame < state.previous.evidence.arrivalSessionFrame) return;
    if (evidence.arrivalSessionFrame == state.previous.evidence.arrivalSessionFrame) {
        if (evidence.frameOfDay != state.previous.evidence.frameOfDay) state.invalid = true;
        return;
    }

    int64_t dayFrames = 0;
    int64_t continuityBoundUs = 0;
    if (!framesPerDay(evidence, &dayFrames) ||
        !combinedObservationBound(state.previous.evidence, evidence, &continuityBoundUs)) {
        state.invalid = true;
        return;
    }

    const int64_t previousDay = state.previous.unwrappedFrame / dayFrames;
    int candidateCount = 0;
    int64_t selectedFrame = 0;
    for (int shift = -1; shift <= 1; ++shift) {
        const I128 candidateValue =
            I128(evidence.frameOfDay) + I128(previousDay + shift) * dayFrames;
        if (candidateValue < state.previous.unwrappedFrame ||
            candidateValue > I128(std::numeric_limits<int64_t>::max()))
            continue;
        const int64_t candidate = int64_t(candidateValue);

        Rational labelProgress;
        Rational sessionProgress;
        Rational residual;
        if (!timeUs(candidate - state.previous.unwrappedFrame, evidence.labelRate,
                    &labelProgress) ||
            !timeUs(evidence.arrivalSessionFrame - state.previous.evidence.arrivalSessionFrame,
                    evidence.sessionRate, &sessionProgress) ||
            !subtract(sessionProgress, labelProgress, &residual)) {
            state.invalid = true;
            return;
        }
        if (!absoluteWithinUs(residual, continuityBoundUs)) continue;
        ++candidateCount;
        selectedFrame = candidate;
    }

    if (candidateCount != 1) {
        state.invalid = true;
        return;
    }
    state.previous = Observation{evidence, selectedFrame};
}

bool TimecodeAlignerV2::hasTimecode(int source) const {
    return source >= 0 && source < kMaxSources && m_sources[source].anchorSet &&
           !m_sources[source].invalid;
}

AlignmentOffset TimecodeAlignerV2::offset(int sourceA, int sourceB, int32_t driftPpm) const {
    AlignmentOffset out;
    if (!hasTimecode(sourceA) || !hasTimecode(sourceB)) return out;
    const SourceState& stateA = m_sources[sourceA];
    const SourceState& stateB = m_sources[sourceB];
    const Observation& anchorA = stateA.anchor;
    const Observation& anchorB = stateB.anchor;

    Rational sessionA;
    Rational sessionB;
    Rational sessionDelta;
    if (!timeUs(anchorA.evidence.arrivalSessionFrame, anchorA.evidence.sessionRate, &sessionA) ||
        !timeUs(anchorB.evidence.arrivalSessionFrame, anchorB.evidence.sessionRate, &sessionB) ||
        !subtract(sessionB, sessionA, &sessionDelta))
        return out;

    int64_t dayFramesB = 0;
    if (!framesPerDay(anchorB.evidence, &dayFramesB)) return out;

    bool selected = false;
    bool ambiguous = false;
    Rational selectedLabelDelta;
    Rational selectedResidual;
    constexpr int64_t kHalfDayUs = 12LL * 60 * 60 * 1'000'000;
    for (int shift = -1; shift <= 1; ++shift) {
        const I128 candidateValue = I128(anchorB.evidence.frameOfDay) + I128(shift) * dayFramesB;
        if (candidateValue < I128(std::numeric_limits<int64_t>::min()) ||
            candidateValue > I128(std::numeric_limits<int64_t>::max()))
            continue;

        Rational labelA;
        Rational labelB;
        Rational labelDelta;
        Rational residual;
        if (!timeUs(anchorA.unwrappedFrame, anchorA.evidence.labelRate, &labelA) ||
            !timeUs(int64_t(candidateValue), anchorB.evidence.labelRate, &labelB) ||
            !subtract(labelB, labelA, &labelDelta) ||
            !subtract(sessionDelta, labelDelta, &residual))
            return out;
        if (!absoluteWithinUs(residual, kHalfDayUs, true)) continue;

        if (!selected) {
            selected = true;
            selectedLabelDelta = labelDelta;
            selectedResidual = residual;
            continue;
        }
        int comparison = 0;
        if (!compareAbsolute(residual, selectedResidual, &comparison)) return out;
        if (comparison < 0) {
            selectedLabelDelta = labelDelta;
            selectedResidual = residual;
            ambiguous = false;
        } else if (comparison == 0) {
            ambiguous = true;
        }
    }
    if (!selected || ambiguous) return out;

    Rational exactOffset;
    if (!subtract(selectedLabelDelta, sessionDelta, &exactOffset) ||
        !roundToInt64(exactOffset, &out.offsetUs))
        return out;

    int64_t evidenceBoundUs = 0;
    if (!combinedObservationBound(anchorA.evidence, anchorB.evidence, &evidenceBoundUs)) return out;
    I128 boundUs = evidenceBoundUs;
    const I128 driftMagnitude = driftPpm < 0 ? -I128(driftPpm) : I128(driftPpm);
    if (driftMagnitude > 0) {
        I128 residualBoundUs = 0;
        if (!ceilScaledAbsolute(selectedLabelDelta, driftMagnitude, 1'000'000, &residualBoundUs) ||
            !checkedAdd(boundUs, residualBoundUs, &boundUs))
            return out;
    }
    if (boundUs < 0 || boundUs > I128(std::numeric_limits<int64_t>::max())) return out;

    out.boundUs = int64_t(boundUs);
    out.sourceGenerationA = stateA.sourceGeneration;
    out.sourceGenerationB = stateB.sourceGeneration;
    out.kind = out.boundUs == 0 ? AlignmentOffset::Kind::Exact : AlignmentOffset::Kind::Bounded;
    return out;
}

void TimecodeAlignerV2::resetSource(int source) {
    if (source < 0 || source >= kMaxSources) return;
    m_sources[source] = SourceState{};
}

void TimecodeAlignerV2::reset() {
    for (int source = 0; source < kMaxSources; ++source)
        resetSource(source);
}
