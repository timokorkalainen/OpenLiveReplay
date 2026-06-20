#ifndef OUTPUTFRAMECLOCK_H
#define OUTPUTFRAMECLOCK_H

#include "playback/output/outputtypes.h"

class OutputFrameClock {
public:
    explicit OutputFrameClock(FrameRate rate);

    FrameRate frameRate() const { return m_rate; }
    qint64 frameIndexToPlayheadMs(qint64 frameIndex) const;
    qint64 samplePlayheadMsForOutputTick(qint64 outputFrameIndex,
                                         const PlaybackStateSnapshot& state) const;
    // Inverse of samplePlayheadMsForOutputTick: the output frame index at which the
    // sampled playhead reaches `playheadMs`, given the play epoch in `state`. Used to
    // fire an armed cut at an exact playhead (e.g. a playlist entry's out-point)
    // rather than as-soon-as-staged. Honors the epoch's speed, so the result is
    // frame-accurate at any playout speed incl. slow-motion. Returns -1 when the
    // clock is not advancing forward (paused, invalid rate, or speed <= 0).
    qint64 outputFrameForPlayheadMs(qint64 playheadMs, const PlaybackStateSnapshot& state) const;

private:
    FrameRate m_rate;
};

#endif // OUTPUTFRAMECLOCK_H
