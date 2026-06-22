#ifndef OUTPUTDISPATCHER_H
#define OUTPUTDISPATCHER_H

#include "playback/output/outputsink.h"

#include <QHash>
#include <QList>
#include <QString>

#include <memory>

class GpuCompositor;
class GpuRhiContext;

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
    qint64 currentQueueDepth = 0;
    qint64 maxQueueDepth = 0;
    qint64 deliveryGaps = 0;
    qint64 lastQueuedFrameIndex = -1;
    qint64 lastDeliveredFrameIndex = -1;
    qint64 lastSubmitDurationNs = 0;
    bool queuePressure = false;
    bool lastSubmitDroppedFrame = false;
    bool lastDeliveryGap = false;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
    qint64 repeatedPayloadFrames = 0;
    bool hasSinkStatus = false;
    bool hasLastSubmitResult = false;
    bool lastSubmitSucceeded = true;
    bool hasLastSinkResult = false;
    bool lastSinkResultSucceeded = true;
    bool hasLastQueuedFrameIndex = false;
    bool hasLastDeliveredFrameIndex = false;
    QString sinkState;
    QString sinkMessage;
    bool hasLastIdentity = false;
    OutputFrameIdentity lastIdentity;
};

struct OutputRuntimeDispatchStats {
    bool hasLastDispatchTiming = false;
    qint64 lastScheduledFrameIndex = -1;
    qint64 lastDispatchedFrameIndex = -1;
    qint64 lastScheduledNs = 0;
    qint64 lastDispatchWallNs = 0;
    qint64 lastLatenessNs = 0;
    qint64 maxLatenessNs = 0;
    qint64 deadlineMisses = 0;
    qint64 catchUpCapHits = 0;
    qint64 cappedCatchUpTicks = 0;
    bool lastDispatchDeadlineMiss = false;
    qint64 lastCappedCatchUpTicks = 0;
};

struct OutputDispatchStats {
    qint64 ticks = 0;
    qint64 framesSubmitted = 0;
    qint64 sinkFailures = 0;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
    qint64 heldFrames = 0;
    qint64 skippedDuplicateFrames = 0;
    // Max |snapshot playhead - sampled (output-clock) playhead| seen while
    // playing, in ms. Detects an un-re-anchored play epoch after a seek/cut: a
    // large value means the output is sampling (and rendering) the WRONG frame
    // even though no placeholder/reposition is reported (frame-accuracy guard).
    qint64 maxClockDivergenceMs = 0;
    qint64 gpuVramBytes = 0;
    qint64 readbackQueueDepth = 0;
    qint64 readbackDrops = 0;
    qint64 fenceWaitStalls = 0;
    qint64 gpuOomDegrades = 0;
    qint64 gpuReadbacks = 0;
    qint64 redundantGpuReadbacks = 0;
    OutputRuntimeDispatchStats runtime;
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
    // The output frame index at which the sampled playhead will reach `playheadMs`,
    // using the current play epoch (honors speed). -1 if no epoch / not advancing
    // forward. Used to fire an armed cut at an exact playhead (a playlist out-point).
    qint64 outputFrameForPlayheadMs(qint64 playheadMs) const;
    void setRuntimeStats(const OutputRuntimeDispatchStats& stats);
    void incrementFenceWaitStalls();
    void setHoldLastFrame(bool enabled) { m_holdLastFrame = enabled; }
    void setIdentitySkip(bool enabled) { m_identitySkip = enabled; }

    OutputDispatchStats dispatchTick(const OutputFrameCache& cache,
                                     const PlaybackStateSnapshot& state);
    OutputDispatchStats stats() const;
    FrameRate frameRate() const { return m_rate; }

private:
    OutputBusFrame renderBus(OutputBusId bus, qint64 outputFrameIndex,
                             const PlaybackStateSnapshot& state, const OutputFrameCache& cache);
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
    MultiviewComposite m_multiviewMemo;
    std::shared_ptr<GpuRhiContext> m_gpuRhi;
    std::shared_ptr<GpuCompositor> m_gpuCompositor;
    bool m_holdLastFrame = true;
    QHash<OutputBusId, OutputBusFrame> m_lastGoodFrame; // per-bus last real video
    bool m_identitySkip = true;
};

#endif // OUTPUTDISPATCHER_H
