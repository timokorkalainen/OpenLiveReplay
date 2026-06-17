#ifndef OUTPUTDISPATCHER_H
#define OUTPUTDISPATCHER_H

#include "playback/output/outputsink.h"

#include <QHash>
#include <QList>
#include <QString>

struct OutputEndpoint {
    OutputTargetAssignment assignment;
    IOutputSink* sink = nullptr; // non-owning; UI/target manager owns sinks
};

struct OutputTargetDispatchStats {
    qint64 attemptedFrames = 0;
    qint64 framesSubmitted = 0;
    qint64 sinkFailures = 0;
    qint64 sinkSubmittedFrames = 0;
    qint64 sinkFailedFrames = 0;
    qint64 sinkDroppedFrames = 0;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
    qint64 repeatedPayloadFrames = 0;
    bool hasSinkStatus = false;
    bool hasLastSubmitResult = false;
    bool lastSubmitSucceeded = true;
    bool hasLastSinkResult = false;
    bool lastSinkResultSucceeded = true;
    QString sinkState;
    QString sinkMessage;
    bool hasLastIdentity = false;
    OutputFrameIdentity lastIdentity;
};

struct OutputDispatchStats {
    qint64 ticks = 0;
    qint64 framesSubmitted = 0;
    qint64 sinkFailures = 0;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
    QHash<QString, OutputTargetDispatchStats> targets;
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
    OutputDispatchStats stats() const;
    FrameRate frameRate() const { return m_rate; }

private:
    OutputBusFrame renderBus(OutputBusId bus, qint64 outputFrameIndex,
                             const PlaybackStateSnapshot& state,
                             const OutputFrameCache& cache) const;
    PlaybackStateSnapshot clockedStateForTick(qint64 outputFrameIndex,
                                              const PlaybackStateSnapshot& state);
    void countFrameHealth(const OutputBusFrame& frame);
    void countTargetStartFailure(const OutputTargetAssignment& assignment);
    void countTargetAttempt(const OutputTargetAssignment& assignment, const OutputBusFrame& frame,
                            bool submitted);

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
