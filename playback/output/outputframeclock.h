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

private:
    FrameRate m_rate;
};

#endif // OUTPUTFRAMECLOCK_H
