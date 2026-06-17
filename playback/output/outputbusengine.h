#ifndef OUTPUTBUSENGINE_H
#define OUTPUTBUSENGINE_H

#include "playback/output/outputframecache.h"
#include "playback/output/outputframeclock.h"
#include "playback/output/outputtargetassignment.h"

#include <QList>

struct OutputBusFrame {
    OutputBusId bus;
    qint64 outputFrameIndex = 0;
    qint64 sampledPlayheadMs = 0;
    MediaVideoFrame video;
    MediaAudioFrame audio;
};

class OutputBusEngine {
public:
    OutputBusEngine(FrameRate rate, int feedCount, int width, int height);

    void setTargetAssignments(const QList<OutputTargetAssignment>& assignments);
    QList<OutputTargetAssignment> targetAssignments() const { return m_assignments; }

    OutputBusFrame renderFeed(int feedIndex, qint64 outputFrameIndex,
                              const PlaybackStateSnapshot& state,
                              const OutputFrameCache& cache) const;
    OutputBusFrame renderPgm(qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
                             const OutputFrameCache& cache) const;
    OutputBusFrame renderMultiview(qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
                                   const OutputFrameCache& cache) const;

    int audioSamplesPerFrame() const;

private:
    OutputBusFrame renderSingleSource(OutputBusId bus, int feedIndex, qint64 outputFrameIndex,
                                      const PlaybackStateSnapshot& state,
                                      const OutputFrameCache& cache, bool allowAudio) const;
    qint64 audioBoundarySampleForFrame(qint64 outputFrameIndex) const;
    qint64 sourceAudioStartSample(qint64 outputFrameIndex,
                                  const PlaybackStateSnapshot& state) const;
    int audioSamplesForOutputFrame(qint64 outputFrameIndex) const;

    OutputFrameClock m_clock;
    int m_feedCount = 0;
    int m_width = 1920;
    int m_height = 1080;
    QList<OutputTargetAssignment> m_assignments;
};

#endif // OUTPUTBUSENGINE_H
