#ifndef TIMECODEALIGNERV2_H
#define TIMECODEALIGNERV2_H

#include "timecodeevidence.h"

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
