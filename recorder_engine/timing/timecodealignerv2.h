#ifndef TIMECODEALIGNERV2_H
#define TIMECODEALIGNERV2_H
#include <cstdint>

// Rate-aware inter-source timecode alignment with a typed Incomparable state.
// Pure (no Qt/FFmpeg). All arithmetic is exact integer (__int128 intermediates),
// so there is no float rounding drift regardless of source/session rate.
//
// This replaces the earlier TimecodeAligner, whose bug was dimensional: it
// counted TC frames at a hardwired nominal 30 fps and differenced them against
// the session-frame axis (which advances at the real session rate). For two
// genuinely aligned sources whose anchors are Δt apart that produced a spurious
// (sessionRate/30 − 1)·(TC-frame difference) term — e.g. two 60p sources 10 s
// apart reported a −300-frame / −5000 ms offset and drove the phase servo the
// wrong way. Carrying each side's true rate and comparing in exact microseconds
// removes the term entirely (see docs/hardest-technical-challenges.md and its
// machine-checked timecode_alignment_proof.py).

// Exact rational frames/second (e.g. 60000/1001). An invalid rate (num<=0 or
// den<=0) is the explicit "rate unrecoverable" sentinel: a source anchored with
// an invalid rate never anchors, so every comparison against it is Incomparable.
struct FrameRateQ {
    int32_t num = 0;
    int32_t den = 1;
    bool valid() const { return num > 0 && den > 0; }
};

struct AlignmentOffset {
    // Comparable  — both sources anchored with valid rates; offsetUs/boundUs hold.
    // Incomparable — a source lacks an anchor or a recoverable rate; offsetUs is
    //                meaningless and MUST NOT be used (callers degrade to the
    //                clock-offset estimate). Never a bare integer.
    enum class Kind : uint8_t { Comparable, Incomparable };
    Kind kind = Kind::Incomparable;
    int64_t offsetUs = 0; // (skewA − skewB): time to ADD to B so equal-TC frames
                          // coincide with A. Sign matches the old frameOffset:
                          // B late (larger skew) => negative => shift B earlier.
    int64_t boundUs = 0;  // proven |measurement error| bound for this pair:
                          // one session frame (arrival quantization) + drift term.

    bool comparable() const { return kind == Kind::Comparable; }
};

class TimecodeAlignerV2 {
public:
    static constexpr int kMaxSources = 16;

    // Record that `source` carried the absolute timecode frame count `tcFrames`
    // (counted AT ITS TRUE LABEL RATE, i.e. Smpte12m::toFrameCount(tc, labelRate)
    // with drop-frame renumbering applied) with true rate `tcRate`, observed on
    // session frame `sessionFrame` (the heartbeat tick count, advancing at
    // `sessionRate`). First observation per source wins (immutable anchor). A
    // negative tcFrames or an invalid rate is ignored -> the source stays
    // unanchored -> Incomparable, never a guess.
    void observe(int source, int64_t tcFrames, FrameRateQ tcRate, int64_t sessionFrame,
                 FrameRateQ sessionRate);

    // Has this source produced at least one usable (valid-rate) anchor?
    bool hasTimecode(int source) const;

    // offset(a,b): the alignment of b relative to a. driftPpm (>=0 magnitude) adds
    // |anchorTcSkew|·ppm to the bound (the residual a drifting session clock leaves
    // that the servo cannot remove). Incomparable unless BOTH sources are anchored.
    AlignmentOffset offset(int a, int b, int32_t driftPpm = 0) const;

    void reset();

private:
    struct Anchor {
        bool set = false;
        int64_t tcFrames = 0;
        FrameRateQ tcRate;
        int64_t sessionFrame = 0;
        FrameRateQ sessionRate;
    };
    Anchor m_anchors[kMaxSources];
};

#endif // TIMECODEALIGNERV2_H
