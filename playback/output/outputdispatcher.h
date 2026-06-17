#ifndef OUTPUTDISPATCHER_H
#define OUTPUTDISPATCHER_H

#include "playback/output/outputsink.h"

#include <QList>

struct OutputEndpoint {
    OutputTargetAssignment assignment;
    IOutputSink* sink = nullptr; // non-owning; UI/target manager owns sinks
};

struct OutputDispatchStats {
    qint64 ticks = 0;
    qint64 framesSubmitted = 0;
    qint64 sinkFailures = 0;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
};

class OutputDispatcher {
public:
    OutputDispatcher(FrameRate rate, int feedCount, int width, int height);
    ~OutputDispatcher();

    void setEndpoints(const QList<OutputEndpoint>& endpoints);
    QList<OutputEndpoint> endpoints() const { return m_endpoints; }

    void resetFrameIndex(qint64 nextOutputFrameIndex = 0);
    void resetPlayEpoch();
    qint64 nextOutputFrameIndex() const { return m_nextOutputFrameIndex; }

    OutputDispatchStats dispatchTick(const OutputFrameCache& cache,
                                     const PlaybackStateSnapshot& state);
    OutputDispatchStats stats() const { return m_stats; }

private:
    OutputBusFrame renderBus(OutputBusId bus, qint64 outputFrameIndex,
                             const PlaybackStateSnapshot& state,
                             const OutputFrameCache& cache) const;
    PlaybackStateSnapshot clockedStateForTick(qint64 outputFrameIndex,
                                              const PlaybackStateSnapshot& state);
    void countFrameHealth(const OutputBusFrame& frame);

    FrameRate m_rate;
    int m_feedCount = 0;
    int m_width = 1920;
    int m_height = 1080;
    QList<OutputEndpoint> m_endpoints;
    qint64 m_nextOutputFrameIndex = 0;
    bool m_havePlayEpoch = false;
    PlaybackStateSnapshot m_playEpoch;
    OutputDispatchStats m_stats;
};

#endif // OUTPUTDISPATCHER_H
