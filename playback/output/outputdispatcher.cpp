#include "playback/output/outputdispatcher.h"

#include <QHash>

namespace {

QString targetStatsKey(const OutputTargetAssignment& assignment) {
    const QString id = assignment.id.trimmed();
    if (!id.isEmpty()) return id;
    return QStringLiteral("%1:%2:%3")
        .arg(int(assignment.kind))
        .arg(int(assignment.sourceBus.kind))
        .arg(assignment.sourceBus.index);
}

bool isSilentAudio(const MediaAudioFrame& audio) {
    for (const char sample : audio.pcm) {
        if (sample != '\0') return false;
    }
    return true;
}

bool hasMeaningfulSinkStatus(const OutputSinkStatus& status) {
    return status.acceptedFrames > 0 || status.failedFrames > 0 || status.droppedFrames > 0 ||
           status.currentQueueDepth > 0 || status.maxQueueDepth > 0 || status.deliveryGaps > 0 ||
           status.lastSubmitDurationNs > 0 || status.hasLastResult || status.queuePressure ||
           status.lastSubmitDroppedFrame || status.lastDeliveryGap ||
           status.hasLastQueuedFrameIndex || status.hasLastDeliveredFrameIndex ||
           !status.state.isEmpty() || !status.message.isEmpty();
}

} // namespace

OutputDispatcher::OutputDispatcher(FrameRate rate, int feedCount, int width, int height)
    : m_rate(rate), m_feedCount(qMax(0, feedCount)), m_width(qMax(2, width)),
      m_height(qMax(2, height)) {}

OutputDispatcher::~OutputDispatcher() {
    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (endpoint.sink) endpoint.sink->stop();
    }
}

void OutputDispatcher::setEndpoints(const QList<OutputEndpoint>& endpoints) {
    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (endpoint.sink) endpoint.sink->stop();
    }

    m_endpoints = endpoints;
    m_multiviewMemo = MultiviewComposite{};
    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (!endpoint.sink || !endpoint.assignment.enabled) continue;
        if (endpoint.sink->kind() != endpoint.assignment.kind) continue;
        if (!endpoint.sink->start(endpoint.assignment, m_rate)) {
            countTargetStartFailure(endpoint.assignment);
        }
    }
}

void OutputDispatcher::resetFrameIndex(qint64 nextOutputFrameIndex) {
    m_nextOutputFrameIndex = qMax<qint64>(0, nextOutputFrameIndex);
    m_havePlayEpoch = false;
}

void OutputDispatcher::resetPlayEpoch() {
    m_havePlayEpoch = false;
}

void OutputDispatcher::setRuntimeStats(const OutputRuntimeDispatchStats& stats) {
    m_stats.runtime = stats;
}

OutputDispatchStats OutputDispatcher::dispatchTick(const OutputFrameCache& cache,
                                                   const PlaybackStateSnapshot& state) {
    const qint64 outputFrameIndex = m_nextOutputFrameIndex++;
    const PlaybackStateSnapshot tickState = clockedStateForTick(outputFrameIndex, state);
    QHash<OutputBusId, OutputBusFrame> rendered;

    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (!endpoint.assignment.enabled || !endpoint.sink || !endpoint.sink->isActive()) continue;

        const OutputBusId bus = endpoint.assignment.sourceBus;
        if (!rendered.contains(bus)) {
            OutputBusFrame frame = renderBus(bus, outputFrameIndex, tickState, cache);
            if (m_holdLastFrame && frame.video.isPlaceholder && m_lastGoodFrame.contains(bus)) {
                // Paint the last real video for this bus instead of the gray
                // placeholder; keep the freshly-rendered audio + identity +
                // outputFrameIndex so the clock and audio timeline never stall.
                frame.video = m_lastGoodFrame.value(bus).video;
                m_stats.heldFrames++;
            } else if (!frame.video.isPlaceholder) {
                m_lastGoodFrame.insert(bus, frame);
            }
            rendered.insert(bus, frame);
            countFrameHealth(rendered.value(bus));
        }

        const OutputBusFrame frame = rendered.value(bus);

        // Identity-skip: if this endpoint already received a byte-identical
        // payload, skip the submit (and the sink's map/copy/deliver entirely).
        OutputTargetDispatchStats& tstats = m_stats.targets[targetStatsKey(endpoint.assignment)];
        if (m_identitySkip && tstats.hasLastIdentity &&
            tstats.lastIdentity.samePayloadAs(frame.identity)) {
            tstats.repeatedPayloadFrames++;
            m_stats.skippedDuplicateFrames++;
            continue;
        }

        const bool submitted = endpoint.sink->submit(frame);
        countTargetAttempt(endpoint.assignment, frame, submitted);
        if (submitted) {
            m_stats.framesSubmitted++;
        } else {
            m_stats.sinkFailures++;
        }
    }

    m_stats.ticks++;
    return m_stats;
}

OutputDispatchStats OutputDispatcher::stats() const {
    OutputDispatchStats snapshot = m_stats;
    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (!endpoint.assignment.enabled || !endpoint.sink ||
            endpoint.sink->kind() != endpoint.assignment.kind) {
            continue;
        }

        const OutputSinkStatus sinkStatus = endpoint.sink->outputStatus();
        if (!hasMeaningfulSinkStatus(sinkStatus)) continue;

        OutputTargetDispatchStats& target = snapshot.targets[targetStatsKey(endpoint.assignment)];
        target.hasSinkStatus = true;
        target.sinkSubmittedFrames = sinkStatus.acceptedFrames;
        target.sinkFailedFrames = sinkStatus.failedFrames;
        target.sinkDroppedFrames = sinkStatus.droppedFrames;
        target.currentQueueDepth = sinkStatus.currentQueueDepth;
        target.maxQueueDepth = sinkStatus.maxQueueDepth;
        target.deliveryGaps = sinkStatus.deliveryGaps;
        target.lastQueuedFrameIndex = sinkStatus.lastQueuedFrameIndex;
        target.lastDeliveredFrameIndex = sinkStatus.lastDeliveredFrameIndex;
        target.lastSubmitDurationNs = sinkStatus.lastSubmitDurationNs;
        target.queuePressure = sinkStatus.queuePressure;
        target.lastSubmitDroppedFrame = sinkStatus.lastSubmitDroppedFrame;
        target.lastDeliveryGap = sinkStatus.lastDeliveryGap;
        target.hasLastSinkResult = sinkStatus.hasLastResult;
        target.lastSinkResultSucceeded = sinkStatus.lastResultSucceeded;
        target.hasLastQueuedFrameIndex = sinkStatus.hasLastQueuedFrameIndex;
        target.hasLastDeliveredFrameIndex = sinkStatus.hasLastDeliveredFrameIndex;
        target.sinkState = sinkStatus.state;
        target.sinkMessage = sinkStatus.message;
    }
    return snapshot;
}

PlaybackStateSnapshot OutputDispatcher::clockedStateForTick(qint64 outputFrameIndex,
                                                            const PlaybackStateSnapshot& state) {
    PlaybackStateSnapshot tickState = state;
    if (!state.playing) {
        m_havePlayEpoch = false;
        return tickState;
    }

    const bool speedChanged =
        m_havePlayEpoch && !qFuzzyCompare(m_playEpoch.speed + 1.0, state.speed + 1.0);
    if (!m_havePlayEpoch || speedChanged) {
        m_playEpoch = state;
        m_playEpoch.playStartedAtOutputFrame = outputFrameIndex;
        m_playEpoch.playStartedAtPlayheadMs = state.playheadMs;
        m_havePlayEpoch = true;
    }

    tickState.playStartedAtOutputFrame = m_playEpoch.playStartedAtOutputFrame;
    tickState.playStartedAtPlayheadMs = m_playEpoch.playStartedAtPlayheadMs;
    return tickState;
}

OutputBusFrame OutputDispatcher::renderBus(OutputBusId bus, qint64 outputFrameIndex,
                                           const PlaybackStateSnapshot& state,
                                           const OutputFrameCache& cache) {
    OutputBusEngine engine(m_rate, m_feedCount, m_width, m_height);
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return engine.renderFeed(bus.index, outputFrameIndex, state, cache);
    case OutputBusKind::Multiview:
        return engine.renderMultiview(outputFrameIndex, state, cache, &m_multiviewMemo);
    case OutputBusKind::Pgm:
        return engine.renderPgm(outputFrameIndex, state, cache);
    }
    return engine.renderPgm(outputFrameIndex, state, cache);
}

void OutputDispatcher::countFrameHealth(const OutputBusFrame& frame) {
    if (frame.video.isPlaceholder) m_stats.placeholderFrames++;
    if (isSilentAudio(frame.audio)) m_stats.silentAudioFrames++;
}

void OutputDispatcher::countTargetStartFailure(const OutputTargetAssignment& assignment) {
    OutputTargetDispatchStats& stats = m_stats.targets[targetStatsKey(assignment)];
    stats.sinkFailures++;
    stats.hasLastSubmitResult = true;
    stats.lastSubmitSucceeded = false;
    m_stats.sinkFailures++;
}

void OutputDispatcher::countTargetAttempt(const OutputTargetAssignment& assignment,
                                          const OutputBusFrame& frame, bool submitted) {
    OutputTargetDispatchStats& stats = m_stats.targets[targetStatsKey(assignment)];
    stats.attemptedFrames++;
    if (submitted) {
        stats.framesSubmitted++;
    } else {
        stats.sinkFailures++;
    }
    stats.hasLastSubmitResult = true;
    stats.lastSubmitSucceeded = submitted;
    if (frame.video.isPlaceholder) stats.placeholderFrames++;
    if (isSilentAudio(frame.audio)) stats.silentAudioFrames++;
    if (stats.hasLastIdentity && stats.lastIdentity.samePayloadAs(frame.identity)) {
        stats.repeatedPayloadFrames++;
    }
    stats.lastIdentity = frame.identity;
    stats.hasLastIdentity = true;
}
