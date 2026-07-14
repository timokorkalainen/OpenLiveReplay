#ifndef TIMECODEALIGNERV2_H
#define TIMECODEALIGNERV2_H

#include "timecodeevidence.h"

// Rate-aware inter-source timecode alignment with immutable, generation-bound
// anchors. Per-source observations are unwrapped across at most one adjacent
// timecode day. Invalid, stale, discontinuous, or arithmetically unsafe state
// is never guessed: it remains explicitly Incomparable.
class TimecodeAlignerV2 {
public:
    static constexpr int kMaxSources = 16;

    // Compatibility adapter for the current ReplayManager producer. Typed
    // evidence replaces this call site in the end-to-end propagation step.
    void observe(int source, int64_t tcFrames, FrameRateQ tcRate, int64_t sessionFrame,
                 FrameRateQ sessionRate);
    void observe(int source, const TimecodeEvidence& evidence);

    bool hasTimecode(int source) const;

    // driftPpm is an optional additional clock-drift magnitude. Its residual
    // over the selected anchor separation is conservatively rounded upward.
    AlignmentOffset offset(int sourceA, int sourceB, int32_t driftPpm = 0) const;

    void resetSource(int source);
    void reset();

private:
    struct Observation {
        TimecodeEvidence evidence;
        int64_t unwrappedFrame = 0;
    };

    struct SourceState {
        bool generationSet = false;
        bool invalid = false;
        bool anchorSet = false;
        uint64_t sourceGeneration = 0;
        uint64_t timingGeneration = 0;
        Observation anchor;
        Observation previous;
    };

    SourceState m_sources[kMaxSources];
};

#endif // TIMECODEALIGNERV2_H
