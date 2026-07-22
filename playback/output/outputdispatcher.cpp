#include "playback/output/outputdispatcher.h"

#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/gpureadbackring.h"
#endif
#include "playback/output/gpureadbacktelemetry.h"
#include "playback/output/outputframeclock.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>

#include <memory>

namespace {

constexpr int kPausedExternalFlushTimeoutMs = 16;
constexpr int kPausedPreviewFlushTimeoutMs = 64;

bool envFlagValue(const char* name, bool* value) {
    const QByteArray raw = qgetenv(name).trimmed().toLower();
    if (raw.isEmpty()) return false;
    if (raw == "0" || raw == "false" || raw == "off" || raw == "no") {
        if (value) *value = false;
        return true;
    }
    if (value) *value = true;
    return true;
}

bool pausedQtPreviewFlushEnabled() {
    bool enabled = false;
    if (envFlagValue("OLR_QT_PREVIEW_SYNC_FLUSH", &enabled)) return enabled;
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}

bool latencyTraceEnabled() {
    bool enabled = false;
    return envFlagValue("OLR_E2E_LATENCY_TRACE", &enabled) && enabled;
}

QString targetStatsKey(const OutputTargetAssignment& assignment) {
    QString id = assignment.id.trimmed();
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

std::shared_ptr<SharedGpuReadbackCache> createSharedReadbackCache() {
#ifdef OLR_GPU_PIPELINE_BUILD
    return std::make_shared<SharedGpuReadbackCache>();
#else
    return nullptr;
#endif
}

bool endpointMatchesRequest(const OutputEndpoint& endpoint, const OutputDispatchRequest& request) {
    if (request.lane == OutputDispatchLane::All) return true;
    if (request.lane == OutputDispatchLane::PgmCritical) {
        return endpoint.assignment.sourceBus == OutputBusId::pgm() &&
               (endpoint.assignment.kind == request.requiredKind ||
                endpoint.assignment.kind == OutputTargetKind::QtPreview);
    }
    if (request.lane == OutputDispatchLane::PreviewFollower) {
        return endpoint.assignment.kind == OutputTargetKind::QtPreview;
    }
    return true;
}

bool submittedFrameMatchesRequest(const OutputTargetAssignment& assignment,
                                  const OutputBusFrame& frame,
                                  const OutputDispatchRequest& request) {
    if (request.lane != OutputDispatchLane::PgmCritical) return false;
    if (!(assignment.sourceBus == request.requiredBus)) return false;
    if (assignment.kind != request.requiredKind) return false;
    if (request.requiredPlayheadMs >= 0 && frame.sampledPlayheadMs != request.requiredPlayheadMs) {
        return false;
    }
    if (request.requireNonPlaceholder && frame.video.metadata().key.isPlaceholder) return false;
    return true;
}

bool endpointMatchesPausedPgmCadence(const OutputEndpoint& endpoint) {
    return endpoint.assignment.sourceBus == OutputBusId::pgm() &&
           endpoint.assignment.kind != OutputTargetKind::QtPreview;
}

int pausedPgmPrewarmOffsetFrames(qint64 outputFrameIndex) {
    static constexpr int kOffsets[] = {-1, 1, -2, 2};
    const int slot = int(outputFrameIndex % qint64(std::size(kOffsets)));
    return kOffsets[slot < 0 ? slot + int(std::size(kOffsets)) : slot];
}

} // namespace

OutputDispatcher::OutputDispatcher(FrameRate rate, int feedCount, int width, int height,
                                   std::shared_ptr<GpuRhiContext> gpuRhi)
    : m_rate(rate), m_feedCount(qMax(0, feedCount)), m_width(qMax(2, width)),
      m_height(qMax(2, height)), m_sharedReadbacks(createSharedReadbackCache()) {
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled()) {
        setGpuRhiContext(gpuRhi ? std::move(gpuRhi) : GpuRhiContext::create());
    }
#else
    Q_UNUSED(gpuRhi);
#endif
}

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
    m_pgmMemo = PgmComposite{};
    m_multiviewMemo = MultiviewComposite{};
    for (auto it = m_stats.targets.begin(); it != m_stats.targets.end(); ++it) {
        it->hasLastIdentity = false;
        it->lastIdentity = OutputFrameIdentity{};
    }
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
    m_lastGoodFrame.clear();
    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (!endpoint.sink || !endpoint.assignment.enabled) continue;
        endpoint.sink->discardPending();
    }
}

qint64 OutputDispatcher::outputFrameForPlayheadMs(qint64 playheadMs) const {
    if (!m_havePlayEpoch) return -1;
    return OutputFrameClock(m_rate).outputFrameForPlayheadMs(playheadMs, m_playEpoch);
}

void OutputDispatcher::setRuntimeStats(const OutputRuntimeDispatchStats& stats) {
    m_stats.runtime = stats;
}

void OutputDispatcher::incrementFenceWaitStalls() {
    m_stats.fenceWaitStalls++;
}

void OutputDispatcher::setGpuRhiContext(std::shared_ptr<GpuRhiContext> gpuRhi) {
#ifdef OLR_GPU_PIPELINE_BUILD
    m_gpuRhi = std::move(gpuRhi);
    m_gpuCompositor = m_gpuRhi && m_gpuRhi->isValid() && !m_gpuRhi->deviceLost()
                          ? GpuCompositor::create(m_gpuRhi)
                          : nullptr;
#else
    Q_UNUSED(gpuRhi);
#endif
}

OutputDispatchStats OutputDispatcher::dispatchTick(const OutputFrameCache& cache,
                                                   const PlaybackStateSnapshot& state,
                                                   OutputDispatchFlushMode flushMode) {
    return dispatchTickWithReport(cache, state, flushMode).stats;
}

void OutputDispatcher::advanceClockOnlyTick(const PlaybackStateSnapshot& state) {
    const qint64 outputFrameIndex = m_nextOutputFrameIndex++;
    clockedStateForTick(outputFrameIndex, state);
    m_stats.ticks++;
}

OutputDispatchReport OutputDispatcher::dispatchTickWithReport(
    const OutputFrameCache& cache, const PlaybackStateSnapshot& state,
    OutputDispatchFlushMode flushMode, const OutputDispatchRequest& request) {
    const bool traceLatency = latencyTraceEnabled();
    OutputDispatchReport report;
    const qint64 outputFrameIndex = m_nextOutputFrameIndex++;
    const PlaybackStateSnapshot tickState = clockedStateForTick(outputFrameIndex, state);
    QHash<OutputBusId, OutputBusFrame> rendered;
    QHash<OutputBusId, qint64> renderDurationsNs;
#ifdef OLR_GPU_PIPELINE_BUILD
    if (m_sharedReadbacks) m_sharedReadbacks->clear();
#endif

    QList<const OutputEndpoint*> endpointOrder;
    endpointOrder.reserve(m_endpoints.size());
    if (flushMode == OutputDispatchFlushMode::PausedPgmCadence) {
        for (const OutputEndpoint& endpoint : m_endpoints) {
            if (endpointMatchesPausedPgmCadence(endpoint) &&
                endpointMatchesRequest(endpoint, request))
                endpointOrder.append(&endpoint);
        }
    } else if (flushMode == OutputDispatchFlushMode::PausedImmediate) {
        for (const OutputEndpoint& endpoint : m_endpoints) {
            if (endpoint.assignment.kind != OutputTargetKind::QtPreview &&
                endpointMatchesRequest(endpoint, request))
                endpointOrder.append(&endpoint);
        }
        for (const OutputEndpoint& endpoint : m_endpoints) {
            if (endpoint.assignment.kind == OutputTargetKind::QtPreview &&
                endpointMatchesRequest(endpoint, request))
                endpointOrder.append(&endpoint);
        }
    } else {
        for (const OutputEndpoint& endpoint : m_endpoints) {
            if (endpointMatchesRequest(endpoint, request)) endpointOrder.append(&endpoint);
        }
    }

    if (flushMode == OutputDispatchFlushMode::PausedPgmCadence && !tickState.playing)
        prewarmPausedPgmCadenceReadback(cache, tickState, outputFrameIndex, endpointOrder);

    for (const OutputEndpoint* endpointPtr : endpointOrder) {
        const OutputEndpoint& endpoint = *endpointPtr;
        if (!endpoint.assignment.enabled || !endpoint.sink || !endpoint.sink->isActive()) continue;
        const bool isQtPreview = endpoint.assignment.kind == OutputTargetKind::QtPreview;
        const bool pausedDefaultTick =
            !tickState.playing && flushMode == OutputDispatchFlushMode::Default;
        if (pausedDefaultTick && !isQtPreview) {
            if (traceLatency) {
                qInfo().noquote()
                    << QStringLiteral("OLR_LATENCY output.dispatcher.skip endpoint=%1 kind=%2 "
                                      "frameIndex=%3 playheadMs=%4 reason=paused-default-external")
                           .arg(endpoint.assignment.id)
                           .arg(int(endpoint.assignment.kind))
                           .arg(outputFrameIndex)
                           .arg(tickState.playheadMs);
            }
            continue;
        }

        const OutputBusId bus = endpoint.assignment.sourceBus;
        if (!rendered.contains(bus)) {
            QElapsedTimer renderTimer;
            if (traceLatency) renderTimer.start();
            OutputBusFrame frame = renderBus(bus, outputFrameIndex, tickState, cache);
            if (traceLatency) renderDurationsNs.insert(bus, renderTimer.nsecsElapsed());
            if (m_holdLastFrame && frame.video.metadata().key.isPlaceholder &&
                m_lastGoodFrame.contains(bus)) {
                const OutputBusFrame held = m_lastGoodFrame.value(bus);
                if (held.video.isStaleForGeneration(tickState.gpuGeneration)) {
                    m_lastGoodFrame.remove(bus);
                } else {
                    // Paint the last real video for this bus instead of the gray
                    // placeholder; keep the freshly-rendered audio + identity +
                    // outputFrameIndex so the clock and audio timeline never stall.
                    frame.video = held.video;
                    frame.identity = outputFrameIdentityFor(frame);
                    m_stats.heldFrames++;
                }
            } else if (!frame.video.metadata().key.isPlaceholder) {
                if (frame.video.isStaleForGeneration(tickState.gpuGeneration))
                    m_lastGoodFrame.remove(bus);
                else
                    m_lastGoodFrame.insert(bus, frame);
            }
            rendered.insert(bus, frame);
            countFrameHealth(rendered.value(bus));
            // Frame-accuracy guard: while playing, the sampled (output-clock)
            // playhead must track the snapshot playhead. A large divergence means
            // the play epoch was not re-anchored after a seek/cut and the output
            // is rendering the wrong frame (no placeholder/reposition reported).
            if (tickState.playing && !frame.video.metadata().key.isPlaceholder) {
                const qint64 d = tickState.playheadMs - frame.sampledPlayheadMs;
                const qint64 ad = d < 0 ? -d : d;
                if (ad > m_stats.maxClockDivergenceMs) m_stats.maxClockDivergenceMs = ad;
            }
        }

        const OutputBusFrame frame = rendered.value(bus);

        // Identity-skip: if this endpoint already received a byte-identical
        // payload, skip the submit (and the sink's map/copy/deliver entirely).
        OutputTargetDispatchStats& tstats = m_stats.targets[targetStatsKey(endpoint.assignment)];
        const bool sinkNeedsContinuousCadence = endpoint.sink->needsContinuousCadence();
        qint64 readbackDepth = 0;
        qint64 readbackDrops = 0;
        const bool hasReadbackQueue = endpoint.sink->readbackStats(readbackDepth, readbackDrops);
        Q_UNUSED(readbackDrops);
        const bool pausedPgmCadenceExternal =
            !tickState.playing && flushMode == OutputDispatchFlushMode::PausedPgmCadence &&
            !isQtPreview;
        const bool pausedNdiDuplicateMaySkip = !pausedPgmCadenceExternal && !tickState.playing &&
                                               endpoint.assignment.kind == OutputTargetKind::Ndi &&
                                               sinkNeedsContinuousCadence &&
                                               (!hasReadbackQueue || readbackDepth == 0);
        const bool pausedImmediateExternal =
            !tickState.playing && flushMode == OutputDispatchFlushMode::PausedImmediate &&
            !isQtPreview;
        const bool pausedImmediatePreview = !tickState.playing &&
                                            flushMode == OutputDispatchFlushMode::PausedImmediate &&
                                            isQtPreview;
        const bool duplicateMaySkip = !pausedImmediateExternal && !pausedImmediatePreview &&
                                      (!sinkNeedsContinuousCadence || pausedNdiDuplicateMaySkip);
        if (m_identitySkip && duplicateMaySkip && tstats.hasLastIdentity &&
            tstats.lastIdentity.samePayloadAs(frame.identity)) {
            tstats.repeatedPayloadFrames++;
            m_stats.skippedDuplicateFrames++;
            continue;
        }

        const bool shouldFlushPausedPreview =
            isQtPreview && flushMode == OutputDispatchFlushMode::PausedImmediate &&
            request.lane != OutputDispatchLane::PgmCritical && pausedQtPreviewFlushEnabled();
        const bool shouldFlushPausedImmediate =
            flushMode == OutputDispatchFlushMode::PausedImmediate && !isQtPreview;
        const bool shouldFlush =
            !tickState.playing && (shouldFlushPausedPreview || shouldFlushPausedImmediate);
        const int timeoutMs = endpoint.assignment.kind == OutputTargetKind::QtPreview
                                  ? kPausedPreviewFlushTimeoutMs
                                  : kPausedExternalFlushTimeoutMs;
        QElapsedTimer submitTimer;
        if (traceLatency) submitTimer.start();
        const bool submitted = shouldFlush ? endpoint.sink->submitAndFlush(frame, timeoutMs)
                                           : endpoint.sink->submit(frame);
        const qint64 submitNs = traceLatency ? submitTimer.nsecsElapsed() : 0;
        report.submittedFrames.append(
            OutputSubmittedFrame{endpoint.assignment, frame.identity, submitted, submitNs});
        if (submitted && submittedFrameMatchesRequest(endpoint.assignment, frame, request)) {
            report.requiredSubmitted = true;
            report.requiredIdentity = frame.identity;
        }
        countTargetAttempt(endpoint.assignment, frame, submitted);
        if (traceLatency && flushMode == OutputDispatchFlushMode::PausedImmediate) {
            qInfo().noquote()
                << QStringLiteral(
                       "OLR_LATENCY output.dispatcher endpoint=%1 kind=%2 busKind=%3 busIndex=%4 "
                       "frameIndex=%5 playheadMs=%6 sampledMs=%7 sourcePtsMs=%8 sourceFeed=%9 "
                       "placeholder=%10 gpuBacked=%11 shouldFlush=%12 timeoutMs=%13 renderNs=%14 "
                       "submitNs=%15 submitted=%16")
                       .arg(endpoint.assignment.id)
                       .arg(int(endpoint.assignment.kind))
                       .arg(int(bus.kind))
                       .arg(bus.index)
                       .arg(outputFrameIndex)
                       .arg(tickState.playheadMs)
                       .arg(frame.sampledPlayheadMs)
                       .arg(frame.identity.sourcePtsMs)
                       .arg(frame.identity.sourceFeedIndex)
                       .arg(frame.video.metadata().key.isPlaceholder ? 1 : 0)
                       .arg(frame.video.isGpuBacked() ? 1 : 0)
                       .arg(shouldFlush ? 1 : 0)
                       .arg(timeoutMs)
                       .arg(renderDurationsNs.value(bus, 0))
                       .arg(submitNs)
                       .arg(submitted ? 1 : 0);
        }
        if (submitted) {
            m_stats.framesSubmitted++;
        } else {
            m_stats.sinkFailures++;
        }
    }

    collectReadbackStats(m_stats);

    const GpuReadbackTelemetrySnapshot gpu = GpuReadbackTelemetry::instance().snapshot();
    m_stats.gpuReadbacks = gpu.gpuReadbacks;
    m_stats.uniqueGpuReadbackSurfaces = gpu.uniqueSurfaces;
    m_stats.redundantGpuReadbacks = gpu.redundantReadbacks;

    m_stats.ticks++;
    report.stats = m_stats;
    return report;
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
    collectReadbackStats(snapshot);
    const GpuReadbackTelemetrySnapshot gpu = GpuReadbackTelemetry::instance().snapshot();
    snapshot.gpuReadbacks = gpu.gpuReadbacks;
    snapshot.uniqueGpuReadbackSurfaces = gpu.uniqueSurfaces;
    snapshot.redundantGpuReadbacks = gpu.redundantReadbacks;
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
    if (state.forcePlayEpochReset) m_havePlayEpoch = false;
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
#ifdef OLR_GPU_PIPELINE_BUILD
    engine.setGpuCompositor(m_gpuCompositor);
#endif
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return engine.renderFeed(bus.index, outputFrameIndex, state, cache);
    case OutputBusKind::Multiview:
        return engine.renderMultiview(outputFrameIndex, state, cache, &m_multiviewMemo);
    case OutputBusKind::Pgm:
        return engine.renderPgm(outputFrameIndex, state, cache, &m_pgmMemo);
    }
    return engine.renderPgm(outputFrameIndex, state, cache, &m_pgmMemo);
}

void OutputDispatcher::countFrameHealth(const OutputBusFrame& frame) {
    if (frame.video.metadata().key.isPlaceholder) m_stats.placeholderFrames++;
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
    if (frame.video.metadata().key.isPlaceholder) stats.placeholderFrames++;
    if (isSilentAudio(frame.audio)) stats.silentAudioFrames++;
    if (submitted) {
        if (stats.hasLastIdentity && stats.lastIdentity.samePayloadAs(frame.identity)) {
            stats.repeatedPayloadFrames++;
        }
        stats.lastIdentity = frame.identity;
        stats.hasLastIdentity = true;
    }
}

void OutputDispatcher::prewarmPausedPgmCadenceReadback(
    const OutputFrameCache& cache, const PlaybackStateSnapshot& state, qint64 outputFrameIndex,
    const QList<const OutputEndpoint*>& endpoints) {
    if (!m_rate.isValid()) return;
    const int offsetFrames = pausedPgmPrewarmOffsetFrames(outputFrameIndex);
    const qint64 offsetMs = m_rate.frameIndexToMs(qAbs(offsetFrames));
    if (offsetMs <= 0) return;

    PlaybackStateSnapshot prewarmState = state;
    prewarmState.playing = false;
    prewarmState.playheadMs = offsetFrames < 0 ? qMax<qint64>(0, state.playheadMs - offsetMs)
                                               : state.playheadMs + offsetMs;
    if (prewarmState.playheadMs == state.playheadMs) return;

    OutputBusEngine engine(m_rate, m_feedCount, m_width, m_height);
#ifdef OLR_GPU_PIPELINE_BUILD
    engine.setGpuCompositor(m_gpuCompositor);
#endif
    PgmComposite prewarmMemo;
    const OutputBusFrame frame =
        engine.renderPgm(outputFrameIndex, prewarmState, cache, &prewarmMemo);
    if (frame.video.metadata().key.isPlaceholder) return;

    for (const OutputEndpoint* endpoint : endpoints) {
        if (!endpoint || !endpoint->assignment.enabled || !endpoint->sink ||
            !endpoint->sink->isActive())
            continue;
        if (!endpointMatchesPausedPgmCadence(*endpoint)) continue;
        endpoint->sink->prewarmReadback(frame);
    }
}

void OutputDispatcher::collectReadbackStats(OutputDispatchStats& stats) const {
    stats.readbackQueueDepth = 0;
    stats.readbackDrops = 0;
    for (const OutputEndpoint& endpoint : m_endpoints) {
        if (!endpoint.assignment.enabled || !endpoint.sink ||
            endpoint.sink->kind() != endpoint.assignment.kind) {
            continue;
        }

        qint64 depth = 0;
        qint64 drops = 0;
        if (!endpoint.sink->readbackStats(depth, drops)) continue;
        stats.readbackQueueDepth = qMax(stats.readbackQueueDepth, depth);
        stats.readbackDrops += drops;
    }
}
