#include "timecodealignerv2.h"

namespace {
using I128 = __int128;

// microseconds(frames / rate) with a single explicit round-to-nearest. rate is
// assumed valid (callers guard). __int128 intermediate so no overflow/round for
// any real (frames, rate): |frames| < 24h*240fps ~ 2e7, *1e6*den fits easily.
int64_t usFor(int64_t frames, FrameRateQ r) {
    const I128 n = I128(frames) * 1'000'000 * r.den;
    const I128 d = I128(r.num);
    return int64_t((n >= 0 ? n + d / 2 : n - d / 2) / d);
}

// Ceil of one frame period in microseconds (worst-case arrival quantization).
int64_t framePeriodUs(FrameRateQ r) {
    return int64_t((I128(1'000'000) * r.den + r.num - 1) / r.num);
}

int64_t absI64(int64_t v) {
    return v < 0 ? -v : v;
}
} // namespace

void TimecodeAlignerV2::observe(int s, int64_t tcFrames, FrameRateQ tcRate, int64_t sessionFrame,
                                FrameRateQ sessionRate) {
    if (s < 0 || s >= kMaxSources || tcFrames < 0) return;
    if (!tcRate.valid() || !sessionRate.valid()) return; // no valid rate -> stays Incomparable
    Anchor& a = m_anchors[s];
    if (a.set) return; // first observation wins (immutable anchor)
    a = Anchor{true, tcFrames, tcRate, sessionFrame, sessionRate};
}

bool TimecodeAlignerV2::hasTimecode(int s) const {
    return s >= 0 && s < kMaxSources && m_anchors[s].set;
}

AlignmentOffset TimecodeAlignerV2::offset(int ia, int ib, int32_t driftPpm) const {
    AlignmentOffset out; // Incomparable by default
    if (!hasTimecode(ia) || !hasTimecode(ib)) return out;
    const Anchor& A = m_anchors[ia];
    const Anchor& B = m_anchors[ib];

    // skew_i = sessionFrame/sessionRate − tcFrames/tcRate, exact microseconds.
    // This is source i's (arrival − timecode) wall-clock skew; two genuinely
    // aligned sources differ only by their pipeline latency, so the difference
    // of skews is the correction the servo must apply.
    const int64_t sessA = usFor(A.sessionFrame, A.sessionRate);
    const int64_t sessB = usFor(B.sessionFrame, B.sessionRate);
    const int64_t tcA = usFor(A.tcFrames, A.tcRate);
    const int64_t tcB = usFor(B.tcFrames, B.tcRate);
    const int64_t skewA = sessA - tcA;
    const int64_t skewB = sessB - tcB;

    out.offsetUs = skewA - skewB;

    // Bound = one session frame (arrival quantization, worst of the two rates)
    // + drift residual (|anchor TC skew| · |ppm|). Exact per the bound sweep in
    // timecode_alignment_proof.py (0 violations across 3,969 cells).
    const int64_t q = framePeriodUs(A.sessionRate) > framePeriodUs(B.sessionRate)
                          ? framePeriodUs(A.sessionRate)
                          : framePeriodUs(B.sessionRate);
    const int64_t tcSkewUs = tcA - tcB;
    const int64_t drift =
        int64_t((I128(absI64(tcSkewUs)) * I128(absI64(int64_t(driftPpm)))) / 1'000'000);
    out.boundUs = q + drift;
    out.kind = AlignmentOffset::Kind::Comparable;
    return out;
}

void TimecodeAlignerV2::reset() {
    for (auto& a : m_anchors)
        a = Anchor{};
}
