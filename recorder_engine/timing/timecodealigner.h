#ifndef TIMECODEALIGNER_H
#define TIMECODEALIGNER_H
#include "smpte12m.h"
#include <cstdint>

// Per-source timecode tracker + inter-source aligner. Pure (no Qt/FFmpeg).
// "Frame index" is the session frame index (heartbeat tick count) at which a
// given timecode arrived; the aligner remembers the TC<->frame relation so any
// later TC can be mapped, and two sources' relations can be differenced.
class TimecodeAligner {
public:
    explicit TimecodeAligner(int nominalFps = 30);

    // Record that source TC (100 ns since midnight, or -1 = none) arrived on the
    // session frame `sessionFrameIndex`. -1 timecodes are ignored.
    void observe(int sourceIndex, int64_t sourceTimecode100ns, int64_t sessionFrameIndex);

    // Has this source produced at least one usable timecode?
    bool hasTimecode(int sourceIndex) const;

    // The session frame index this source's TC maps to. Returns -1 if the source
    // never carried TC. mediaTimecode100ns is the frame's own TC.
    int64_t toSessionFrameIndex(int sourceIndex, int64_t mediaTimecode100ns) const;

    // True iff both sources carry TC AND their TC<->frame relations agree to
    // within `toleranceFrames` (i.e. equal-TC frames coincide). When false but
    // both have TC, frameOffset() gives the correction.
    bool sourcesAligned(int sourceIndexA, int sourceIndexB, int toleranceFrames = 0) const;

    // Frames to ADD to source B's mapping so equal-TC frames coincide with A
    // (B leads -> positive). 0 if either lacks TC.
    int64_t frameOffset(int sourceIndexA, int sourceIndexB) const;

    void reset();

private:
    struct Anchor { bool set = false; int64_t tcFrames = 0; int64_t sessionFrame = 0; };
    int m_nominalFps;
    static constexpr int kMaxSources = 16;
    Anchor m_anchors[kMaxSources];
};
#endif // TIMECODEALIGNER_H
