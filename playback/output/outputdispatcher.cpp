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

OutputDispatchStats OutputDispatcher::dispatchTick(const OutputFrameCache& cache,
                                                   const PlaybackStateSnapshot& state) {
    const qint64 outputFrameIndex = m_nextOutputFrameIndex++;
    const PlaybackStateSnapshot tickState = clockedStateForTick(outputFrameIndex, state);
    QHash<OutputBusId, OutputBusFrame> rendered;

    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (!endpoint.assignment.enabled || !endpoint.sink || !endpoint.sink->isActive()) continue;

        const OutputBusId bus = endpoint.assignment.sourceBus;
        if (!rendered.contains(bus)) {
            rendered.insert(bus, renderBus(bus, outputFrameIndex, tickState, cache));
            countFrameHealth(rendered.value(bus));
        }

        const OutputBusFrame frame = rendered.value(bus);
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
                                           const OutputFrameCache& cache) const {
    OutputBusEngine engine(m_rate, m_feedCount, m_width, m_height);
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return engine.renderFeed(bus.index, outputFrameIndex, state, cache);
    case OutputBusKind::Multiview:
        return engine.renderMultiview(outputFrameIndex, state, cache);
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
    if (frame.video.isPlaceholder) stats.placeholderFrames++;
    if (isSilentAudio(frame.audio)) stats.silentAudioFrames++;
    if (stats.hasLastIdentity && stats.lastIdentity.samePayloadAs(frame.identity)) {
        stats.repeatedPayloadFrames++;
    }
    stats.lastIdentity = frame.identity;
    stats.hasLastIdentity = true;
}
