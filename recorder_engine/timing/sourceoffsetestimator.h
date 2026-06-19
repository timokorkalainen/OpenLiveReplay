#ifndef SOURCEOFFSETESTIMATOR_H
#define SOURCEOFFSETESTIMATOR_H
#include "sourceclock.h" // ClockQuality
#include <cstdint>

// Confidence in a source's inter-camera phase alignment, ascending. Surfaced to
// the operator: FrameAccurate = common TC or an external reference; Bounded =
// recovered-clock estimate with a numeric +/-ms bound; Approximate = arrival-only.
enum class ConfidenceTier { Approximate = 0, Bounded = 1, FrameAccurate = 2 };

// The evidence the engine feeds per source per update. Pure inputs so the grader
// is fully unit-testable.
struct SourcePhaseEvidence {
    ClockQuality clockQuality = ClockQuality::Arrival;
    bool clockLocked = false;
    bool timecodeAlignedToReference = false; // TimecodeAligner says equal-TC frames coincide
    bool externalReference = false;          // a real reference (PTP) is locked (Phase 5)
    double clockPpm = 0.0;                   // from the source's DriftEstimator
    int64_t measuredOffsetMs = 0;            // measured phase vs the reference source
};

// Per-source offset-to-reference + confidence tier. Pure (no Qt/FFmpeg).
class SourceOffsetEstimator {
public:
    explicit SourceOffsetEstimator(int boundedBaseMs = 4, int boundedPpmMsPerSec = 0);

    void update(int sourceIndex, const SourcePhaseEvidence& ev);

    ConfidenceTier tier(int sourceIndex) const;
    int64_t offsetMs(int sourceIndex) const; // measured phase vs the reference
    // The +/-ms bound for a Bounded tier (0 for FrameAccurate; a wide default for
    // Approximate). Surfaced verbatim as the operator's "how good is it" number.
    int boundMs(int sourceIndex) const;
    bool hasEvidence(int sourceIndex) const;
    void reset();

private:
    struct Entry {
        bool set = false;
        ConfidenceTier tier = ConfidenceTier::Approximate;
        int64_t offsetMs = 0;
        int boundMs = 0;
    };
    static constexpr int kMaxSources = 16;
    static constexpr int kApproximateBoundMs = 40; // arrival jitter order
    static constexpr int kFlvExtraBoundMs = 2;     // ms-resolution FLV is noisier
    int m_boundedBaseMs;
    int m_boundedPpmMsPerSec;
    Entry m_entries[kMaxSources];
};
#endif // SOURCEOFFSETESTIMATOR_H
