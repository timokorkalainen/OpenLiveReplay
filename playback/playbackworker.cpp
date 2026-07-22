#include "playback/playbackworker.h"
#include "playback/cutschedule.h"
#include "playback/output/broadcastoutputsettings.h"
#include "playback/output/colormetadatapolicy.h"
#include "playback/output/iotargets/iotargetsinkfactory.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputframecache.h"
#include "playback/output/ndisink.h"
#include "playback/output/qtpreviewsink.h"
#include "playback/output/queuedoutputsink.h"
#include "playback/output/sinkgpucapability.h"
#include "recorder_engine/ingest/colorvui.h"
#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/output/asyncgpureadbacksink.h"
#include "playback/gpu/decodedonefence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/iosgpulifecyclesink.h"
#include "playback/gpu/iosmemoryheadroom.h"
#include "playback/gpu/iosgpupolicy.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpuseekprefetch.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusurfaceallocator.h"
#ifdef __APPLE__
#include "playback/gpu/appleiosurface.h"
#endif
#ifdef _WIN32
#include "playback/output/win/d3d11gpusurface.h"
#include "playback/output/win/wingpuimportedge.h"
#endif
#endif
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QScopeGuard>
#ifdef OLR_UNIT_TEST
#include <QSemaphore>
#endif
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace {

int64_t liveGrowthFileSizeFromProbe(int64_t avioSize, const QString& filePath) {
    int64_t best = avioSize;
    if (!filePath.isEmpty()) {
        const QFileInfo info(filePath);
        if (info.exists() && info.isFile()) {
            best = qMax<int64_t>(best, info.size());
        }
    }
    return best;
}

int64_t liveEofRecoveryAnchorMs(qint64 playheadMs, qint64 newestBeforeEofMs, qint64 trailMs,
                                qint64 frameDurationMs) {
    const qint64 playheadTrailAnchor = qMax<qint64>(0, playheadMs - qMax<qint64>(0, trailMs));
    if (newestBeforeEofMs < 0) return playheadTrailAnchor;

    const qint64 newestTailAnchor =
        qMax<qint64>(0, newestBeforeEofMs - qMax<qint64>(1, frameDurationMs));
    return qMax(playheadTrailAnchor, newestTailAnchor);
}

int64_t steadyClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool liveReadDeadlineInterruptsFromProbe(bool baseInterrupt, int64_t deadlineMs, int64_t nowMs) {
    return baseInterrupt || (deadlineMs >= 0 && nowMs >= deadlineMs);
}

bool latencyTraceEnabled() {
    const QByteArray raw = qgetenv("OLR_E2E_LATENCY_TRACE").trimmed().toLower();
    return !(raw.isEmpty() || raw == "0" || raw == "false" || raw == "off" || raw == "no");
}

} // namespace

#if defined(OLR_GPU_PIPELINE_BUILD) && (defined(__APPLE__) || defined(_WIN32))
namespace {

ColorMetadata colorMetadataForNativeTrack(const DecoderTrack* track) {
    VuiColorInfo vui;
    if (track && !track->h264ParamSets.h264Sps.isEmpty()) {
        vui = parseSpsColorVui(NativeVideoCodec::H264, track->h264ParamSets.h264Sps.first());
    }
    return resolveColorMetadata(vui, track ? track->codecHeight : 0, 2, 2, 2, 2);
}

} // namespace
#endif

#ifdef OLR_GPU_PIPELINE_BUILD
namespace {

constexpr qint64 kMiB = qint64(1024) * 1024;
constexpr qint64 kGiB = 1024 * kMiB;
constexpr qint64 kPressurePollMs = 250;
constexpr qint64 kMintedBytesPressureSample = 64 * kMiB;
constexpr qint64 kSecondWarningLatchMs = 10000;
constexpr qint64 kMaxSaneAvailableMemoryBytes = 256 * kGiB;

std::optional<FrameHandle> cachedCpuSnapshotForDeviceLoss(const FrameHandle& frame) {
    if (!frame.isGpuBacked()) return std::nullopt;
    if (!frame.data()) return std::nullopt;

    CpuPlanes planes = frame.data()->cachedCpuPlanes(FramePixelFormat::Yuv420p);
    if (!planes.isValid()) planes = frame.data()->cachedCpuPlanes(FramePixelFormat::Nv12);
    if (!planes.isValid()) return std::nullopt;

    FrameMetadata meta = frame.metadata();
    meta.gpuGeneration = 0;
    return makeCpuFrameHandle(std::move(planes), meta);
}

void decodeSurfaceGeometryForGpuBudget(const QList<DecoderTrack*>& decoderBank, int fallbackWidth,
                                       int fallbackHeight, int* surfaceWidth, int* surfaceHeight) {
    int width = 0;
    int height = 0;
    for (const DecoderTrack* track : decoderBank) {
        if (!track) continue;
        if (track->codecCtx && track->codecCtx->width > 0 && track->codecCtx->height > 0) {
            width = qMax(width, track->codecCtx->width);
            height = qMax(height, track->codecCtx->height);
        } else if (track->codecWidth > 0 && track->codecHeight > 0) {
            width = qMax(width, track->codecWidth);
            height = qMax(height, track->codecHeight);
        }
    }
    if (width <= 0 || height <= 0) {
        width = fallbackWidth;
        height = fallbackHeight;
    }
    if (surfaceWidth) *surfaceWidth = width;
    if (surfaceHeight) *surfaceHeight = height;
}

} // namespace
#endif

PlaybackWorker::PlaybackWorker(const QList<FrameProvider*>& providers, PlaybackTransport* transport,
                               AudioPlayer* audioPlayer, QObject* parent)
    : QThread(parent) {
    qRegisterMetaType<PlaybackWorker::OperatorSeekResult>("PlaybackWorker::OperatorSeekResult");
    m_transport = transport;
    m_providers = providers;
    m_audioPlayer = audioPlayer;
}

PlaybackWorker::~PlaybackWorker() {
    stop();
    shutdownOutputGraph();
    for (auto* track : m_decoderBank) {
        track->nativeDecoder.reset(); // Tear down VideoToolbox before freeing track
        if (track->codecCtx) avcodec_free_context(&track->codecCtx);
        delete track;
    }
    for (auto* aTrack : m_audioDecoderBank) {
        if (aTrack->codecCtx) avcodec_free_context(&aTrack->codecCtx);
        delete aTrack;
    }
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
    // Tier3 pre-roll: run()'s cleanup clears these on a clean exit; defensively
    // free here too in case the thread never ran (the QVectors are then empty).
    for (auto* track : m_prerollBank) {
        track->nativeDecoder.reset(); // tear down VT/MF session before freeing track
        if (track->codecCtx) avcodec_free_context(&track->codecCtx);
        delete track;
    }
    for (auto* aTrack : m_prerollAudioBank) {
        if (aTrack->codecCtx) avcodec_free_context(&aTrack->codecCtx);
        delete aTrack;
    }
    if (m_prerollFmtCtx) avformat_close_input(&m_prerollFmtCtx);
}

void PlaybackWorker::openFile(const QString& filePath) {
    QMutexLocker locker(&m_mutex);
    m_currentFilePath = filePath;
}

PlaybackWorker::SeekRequestResult PlaybackWorker::requestSeekTo(qint64 timestampMs,
                                                                int directionHint,
                                                                bool registerOperatorTransaction) {
    const int64_t clamped = qMax<int64_t>(0, timestampMs);
    SeekRequestResult result;
    result.clampedTargetMs = clamped;
    QElapsedTimer publishTimer;
    publishTimer.start();
    {
        QMutexLocker locker(&m_mutex);
        // Record travel direction from the current playhead (spec §4/§5/§6.7):
        // drives reverse reposition anchoring, the backward-scrub audio re-prime,
        // and paused dedup direction. m_transport->currentPos() locks the
        // transport's own (independent) mutex, so there is no lock-order concern.
        result.moveDir = directionHint == 0 ? ((clamped >= m_transport->currentPos()) ? 1 : -1)
                                            : (directionHint > 0 ? 1 : -1);
        m_lastMoveDir.store(result.moveDir, std::memory_order_relaxed);
        m_seekTargetMs = clamped;
        // A manual seek supersedes any pending armed-cut work: it cancels a pending
        // decoder-follow (a stale follow to the old cut target would jump the decoder
        // back) and any queued re-arm (the operator's explicit seek is the latest
        // intent — a recall that arrived during an in-flight cut must not fire after
        // it). An ARMED/in-flight cut is cancelled at fire time by maybeFireScheduled
        // Cut via the seek-generation bump below (m_armSeekGen mismatch).
        // Release ordering so a lock-free worker that later observes the new
        // m_seekGeneration (acquire) is guaranteed to also observe these clears.
        m_decoderFollowMs.store(-1, std::memory_order_release);
        m_forwardCutResyncMs.store(-1, std::memory_order_release);
        m_hasPendingRearm.store(false, std::memory_order_release);
        // A new seek target is outstanding until repositionTo commits it. The gate
        // holds the last output-visible playhead until m_committedGeneration catches
        // up. During long steady playback the last reposition target may already be
        // outside the tiny cap window, so refresh the held playhead from the output
        // bookmark before bumping the generation.
        m_committedPlayheadMs.store(m_lastVisiblePlayheadMs.load(std::memory_order_acquire),
                                    std::memory_order_release);
        // Any seek that advances the generation supersedes a still-waiting operator
        // transaction — including local QML scrubs, live-follow and playlist jumps,
        // which bump the generation without registering transactions. Snapshot the
        // orphaned command before overwriting so its completion can be reported.
        quint64 supersededGeneration = 0;
        qint64 supersededTargetMs = -1;
        if (m_operatorSeekCompletion.waiting && !m_operatorSeekCompletion.completed) {
            supersededGeneration = m_operatorSeekCompletion.generation;
            supersededTargetMs = m_operatorSeekCompletion.targetMs;
            m_operatorSeekCompletion.waiting = false;
        }
        // The bump also signals maybeFireScheduledCut to abort an armed cut (manual seek wins).
        result.generation = m_seekGeneration.fetch_add(1, std::memory_order_release) + 1;
        if (registerOperatorTransaction) {
            m_operatorSeekCompletion = OperatorSeekCompletionState{};
            m_operatorSeekCompletion.generation = result.generation;
            m_operatorSeekCompletion.targetMs = clamped;
            m_operatorSeekCompletion.waiting = true;
        }
        OutputCommitResult outputCommit;
        {
            QMutexLocker bufferLocker(&m_bufferMutex);
            uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
            if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
            OutputCommit commit;
            commit.playheadMs = clamped;
            commit.seekGeneration = result.generation;
            commit.gpuGeneration = gpuGeneration;
            commit.cacheAction = OutputCacheAction::Publish;
            commit.coverageMode = OutputCoverageMode::OperatorSeek;
            commit.requireCurrentSeek = false;
            commit.clearSeekTarget = true;
            commit.dispatch = registerOperatorTransaction ? PostCommitDispatch::PgmCritical
                                                          : PostCommitDispatch::Output;
            outputCommit = commitOutputStateLocked(commit);
        }
        if (outputCommit.committed) {
            result.publishNs = publishTimer.nsecsElapsed();
            result.committedFromPublishedCache = true;
            result.dispatch = outputCommit.dispatch;
            // Window reuse served straight from the published output cache (no worker
            // reposition, no reuseAt). Counted under m_mutex, same as reuseSeek.
            m_counters.publishedSeek++;
        } else {
            const bool allowLiveStartupFallback = !registerOperatorTransaction && clamped == 0 &&
                                                  m_transport && m_transport->isPlaying();
            if (allowLiveStartupFallback) {
                QMutexLocker bufferLocker(&m_bufferMutex);
                uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
                if (gpuPipelineEnabled())
                    gpuGeneration = GpuGenerationCounter::instance().current();
#endif
                OutputCommit commit;
                commit.playheadMs = clamped;
                commit.seekGeneration = result.generation;
                commit.gpuGeneration = gpuGeneration;
                commit.cacheAction = OutputCacheAction::Publish;
                commit.coverageMode = OutputCoverageMode::Displayable;
                commit.requireCurrentSeek = false;
                commit.clearSeekTarget = true;
                commit.dispatch = PostCommitDispatch::Output;
                outputCommit = commitOutputStateLocked(commit);
                if (outputCommit.committed) {
                    result.committedFromPublishedCache = true;
                    result.dispatch = outputCommit.dispatch;
                }
            }
            result.publishNs = publishTimer.nsecsElapsed();
        }
        m_workerWake.wakeAll();
        if (supersededGeneration != 0) {
            OperatorSeekResult superseded;
            superseded.completed = false;
            superseded.submittedPgm = false;
            superseded.targetMs = supersededTargetMs;
            superseded.generation = supersededGeneration;
            superseded.message = QStringLiteral("superseded");
            // Queued consumers only; emitting under m_mutex posts an event and returns.
            emit operatorSeekCompleted(supersededGeneration, superseded);
        }
    }
    return result;
}

void PlaybackWorker::seekTo(int64_t timestampMs, int directionHint) {
    const bool traceLatency = latencyTraceEnabled();
    QElapsedTimer traceTimer;
    if (traceLatency) traceTimer.start();
    const SeekRequestResult seek = requestSeekTo(timestampMs, directionHint, false);
    const bool playing = m_transport && m_transport->isPlaying();
    qint64 refreshNs = 0;
    if (seek.dispatch == PostCommitDispatch::Output ||
        (seek.dispatch == PostCommitDispatch::None && playing)) {
        QElapsedTimer refreshTimer;
        if (traceLatency) refreshTimer.start();
        refreshOutputAfterSeekCommit();
        if (traceLatency) refreshNs = refreshTimer.nsecsElapsed();
    }
    if (traceLatency) {
        qInfo().noquote()
            << QStringLiteral(
                   "OLR_LATENCY seekTo targetMs=%1 dir=%2 generation=%3 published=%4 playing=%5 "
                   "publishNs=%6 refreshNs=%7 totalNs=%8")
                   .arg(seek.clampedTargetMs)
                   .arg(seek.moveDir)
                   .arg(static_cast<qulonglong>(seek.generation))
                   .arg(seek.committedFromPublishedCache ? 1 : 0)
                   .arg(playing ? 1 : 0)
                   .arg(seek.publishNs)
                   .arg(refreshNs)
                   .arg(traceTimer.nsecsElapsed());
    }
}

PlaybackWorker::OperatorSeekResult
PlaybackWorker::seekToAndWaitForPgm(qint64 timestampMs, int directionHint, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    const SeekRequestResult seek = requestSeekTo(timestampMs, directionHint, true);

    OperatorSeekResult result;
    result.targetMs = seek.clampedTargetMs;
    result.generation = seek.generation;

    if (seek.committedFromPublishedCache && seek.dispatch == PostCommitDispatch::PgmCritical) {
        const OutputDispatchReport report =
            dispatchPgmCommitObligation(seek.clampedTargetMs, seek.generation);
        completeOperatorSeekTransaction(seek.generation, seek.clampedTargetMs, report);
        refreshPreviewAfterSeekCommit();
    }

    QMutexLocker locker(&m_mutex);
    while (true) {
        if (m_operatorSeekCompletion.generation == seek.generation &&
            m_operatorSeekCompletion.completed) {
            result.submittedPgm = m_operatorSeekCompletion.submittedPgm;
            result.completed = result.submittedPgm;
            result.pgmIdentity = m_operatorSeekCompletion.pgmIdentity;
            result.message = m_operatorSeekCompletion.message;
            result.elapsedNs = timer.nsecsElapsed();
            return result;
        }
        if (m_seekGeneration.load(std::memory_order_acquire) != seek.generation) {
            result.message = QStringLiteral("superseded");
            result.elapsedNs = timer.nsecsElapsed();
            return result;
        }
        const qint64 remainingMs = qint64(timeoutMs) - timer.elapsed();
        if (remainingMs <= 0) {
            // Abandon the transaction so the worker thread does not later complete it and
            // submit PGM for a command the caller has already given up on. m_mutex is held.
            if (m_operatorSeekCompletion.generation == seek.generation &&
                !m_operatorSeekCompletion.completed) {
                m_operatorSeekCompletion.waiting = false;
            }
            result.timedOut = true;
            result.message = QStringLiteral("timed out waiting for PGM output");
            result.elapsedNs = timer.nsecsElapsed();
            return result;
        }
        m_operatorSeekCondition.wait(&m_mutex, static_cast<unsigned long>(remainingMs));
    }
}

quint64 PlaybackWorker::seekToWithPgmNotify(qint64 timestampMs, int directionHint) {
    const SeekRequestResult seek = requestSeekTo(timestampMs, directionHint, true);
    if (seek.committedFromPublishedCache && seek.dispatch == PostCommitDispatch::PgmCritical) {
        // The worker never repositions for an inline-committed generation
        // (requestSeekTo cleared m_seekTargetMs), so the PGM dispatch and the
        // transaction completion are this caller's job — same as the blocking path.
        const OutputDispatchReport report =
            dispatchPgmCommitObligation(seek.clampedTargetMs, seek.generation);
        completeOperatorSeekTransaction(seek.generation, seek.clampedTargetMs, report);
        refreshPreviewAfterSeekCommit();
    }
    return seek.generation;
}

void PlaybackWorker::setActiveAudioView(int viewIndex) {
    int prev = m_activeAudioView.exchange(viewIndex, std::memory_order_relaxed);
    if (prev == viewIndex) return; // no actual change

    // Clear the ring buffer so stale samples from the old view don't
    // bleed into the new one.  We do NOT flush the codec contexts here:
    // they live on the worker thread and flushing from the UI thread is
    // a data race.  Instead, all decoders run continuously (see run()),
    // so the new view's decoder is already warm and ready.
    if (m_audioPlayer) m_audioPlayer->clear();
    // The worker-side AudioFrameQueue is worker-thread-owned; signal the
    // worker to drop it + re-prime (spec §6.7) rather than touch it here.
    m_audioReprime.store(true, std::memory_order_relaxed);
}

void PlaybackWorker::setSelectedOutputFeed(int feedIndex) {
    m_selectedOutputFeed.store(feedIndex, std::memory_order_relaxed);
}

void PlaybackWorker::setRequireAllOutputFeedsForPlayhead(bool required) {
    m_requireAllOutputFeedsForPlayhead.store(required, std::memory_order_release);
}

void PlaybackWorker::setBusPreviewProviders(FrameProvider* multiviewProvider,
                                            FrameProvider* pgmProvider) {
    QMutexLocker locker(&m_mutex);
    m_multiviewPreviewProvider = multiviewProvider;
    m_pgmPreviewProvider = pgmProvider;
    m_outputTargetsDirty.store(true, std::memory_order_relaxed);
}

void PlaybackWorker::setFeedPreviewProvidersEnabled(bool enabled) {
    QMutexLocker locker(&m_mutex);
    m_feedPreviewProvidersEnabled.store(enabled, std::memory_order_release);
    m_outputTargetsDirty.store(true, std::memory_order_relaxed);
}

void PlaybackWorker::setExternalOutputTargets(const QList<OutputTargetAssignment>& assignments) {
    QMutexLocker locker(&m_mutex);
    m_externalOutputAssignments = assignments;
    m_outputTargetsDirty.store(true, std::memory_order_relaxed);
}

void PlaybackWorker::resetOutputPlayEpoch() {
    QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
    if (m_outputRuntime) m_outputRuntime->resetPlayEpoch();
}

std::optional<qint64>
PlaybackWorker::validatedOutputCommitPlayheadLocked(const OutputCommit& commit,
                                                    const OutputFrameCache* coverageCache) const {
    if (commit.requireCurrentSeek &&
        !CommitGate::canCommitReposition(commit.seekGeneration,
                                         m_seekGeneration.load(std::memory_order_acquire),
                                         m_seekTargetMs >= 0)) {
        return std::nullopt;
    }

    qint64 committedPlayheadMs = commit.playheadMs;
    if (m_outputFeedCount > 0) {
        if (!coverageCache) return std::nullopt;
        if (!commit.requireCoverage) {
            // Forced commit (a scheduled cut deferred to its bound): land at the
            // requested playhead without a coverage/displayable gate, so the cut goes
            // to air even if a feed is not yet displayable there.
            return commit.playheadMs;
        }
        // Graph/device recovery may have only placeholders after dead GPU frames
        // are removed. That state is allowed to republish and reset the epoch only
        // while preserving the already committed seek/playhead identity. Real and
        // hold-last commits still have to pass normal generation-aware coverage.
        const bool recoveryCommitPreservesIdentity =
            !commit.requireCurrentSeek && commit.cacheAction == OutputCacheAction::Publish &&
            commit.guardPlayheadCache &&
            commit.seekGeneration == m_committedGeneration.load(std::memory_order_acquire) &&
            commit.playheadMs == m_committedPlayheadMs.load(std::memory_order_acquire);
        const bool placeholderRecoveryCommit =
            recoveryCommitPreservesIdentity &&
            !outputCacheDisplayablePlayheadInCacheLocked(*coverageCache, commit.playheadMs,
                                                         commit.gpuGeneration)
                 .has_value();
        if (placeholderRecoveryCommit) {
            committedPlayheadMs = commit.playheadMs;
        } else if (commit.coverageMode == OutputCoverageMode::Displayable) {
            const std::optional<qint64> displayable = outputCacheDisplayablePlayheadInCacheLocked(
                *coverageCache, commit.playheadMs, commit.gpuGeneration);
            if (!displayable.has_value()) return std::nullopt;
            committedPlayheadMs = *displayable;
        } else if (!outputCacheCoversPlayheadInCacheLocked(*coverageCache, commit.playheadMs,
                                                           commit.gpuGeneration,
                                                           commit.coverageMode)) {
            return std::nullopt;
        }
    }
    return committedPlayheadMs;
}

PlaybackWorker::OutputCommitResult
PlaybackWorker::commitOutputStateLocked(const OutputCommit& commit) {
    // Callers hold m_mutex (when current-seek validation/target clearing is used)
    // and m_bufferMutex in that order. resetOutputPlayEpoch() extends the order to
    // m_outputRuntimeMutex -> OutputRuntime::m_mutex; OutputRuntime::resetPlayEpoch()
    // defers an active-dispatch reset instead of waiting for dispatch completion.
    OutputCommitResult result;
    const OutputFrameCache* coverageCache =
        commit.cacheAction == OutputCacheAction::MergeStagingAndPublish ? m_stagingCache.get()
                                                                        : m_outputCache.get();
    const std::optional<qint64> committedPlayheadMs =
        validatedOutputCommitPlayheadLocked(commit, coverageCache);
    if (!committedPlayheadMs.has_value()) return result;

    switch (commit.cacheAction) {
    case OutputCacheAction::Keep:
        break;
    case OutputCacheAction::Publish:
        publishOutputCacheLocked();
        break;
    case OutputCacheAction::MergeStagingAndPublish:
        if (m_stagingCache) {
            if (!m_outputCache) {
                m_outputCache = std::make_unique<OutputFrameCache>(*m_stagingCache);
            } else {
                OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                m_outputCache->mergeFrom(*m_stagingCache, &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif
            }
        }
        publishOutputCacheLocked();
        break;
    }

    m_committedPlayheadMs.store(*committedPlayheadMs, std::memory_order_relaxed);
    m_lastVisiblePlayheadMs.store(*committedPlayheadMs, std::memory_order_release);
#ifdef OLR_GPU_PIPELINE_BUILD
    m_committedGpuGeneration.store(commit.gpuGeneration, std::memory_order_release);
#endif
    m_outputPlayheadCacheGuarded.store(commit.guardPlayheadCache, std::memory_order_release);
    if (commit.clearSeekTarget) m_seekTargetMs = -1;
    m_committedGeneration.store(commit.seekGeneration, std::memory_order_release);
    resetOutputPlayEpoch();

    if (commit.dispatch == PostCommitDispatch::PgmCritical && m_operatorSeekCompletion.waiting &&
        !m_operatorSeekCompletion.completed &&
        m_operatorSeekCompletion.generation == commit.seekGeneration &&
        m_operatorSeekCompletion.targetMs == *committedPlayheadMs) {
        m_operatorSeekCompletion.pgmDispatchAttempted = true;
    }

    result.committed = true;
    result.committedPlayheadMs = *committedPlayheadMs;
    result.committedGeneration = commit.seekGeneration;
    result.dispatch = commit.dispatch;
    return result;
}

bool PlaybackWorker::operatorPgmObligationAvailableLocked(uint64_t generation) const {
    // Deliberately NOT gated by pgmDispatchAttempted: the per-packet early completion
    // (tryCompleteOperatorSeekFromCurrentOutputCache) stays one-shot via that flag, but
    // the FINAL reposition commit still owes the PGM obligation for the same seek so a
    // transient early miss does not strand the operator's take-to-air. A completed
    // transaction owes nothing, so it cannot double-dispatch.
    return m_operatorSeekCompletion.waiting && !m_operatorSeekCompletion.completed &&
           m_operatorSeekCompletion.generation == generation;
}

PlaybackWorker::OutputCommitResult PlaybackWorker::commitFullRepositionOutputStateLocked(
    const OutputCommit& commit, std::unique_ptr<OutputFrameCache>& liveSaved, qint64 keepFrom,
    qint64 keepTo, qint64 keepAudioFromSample, bool sanitizeForDeviceLoss) {
    OutputCommitResult result;
    if (commit.cacheAction != OutputCacheAction::MergeStagingAndPublish || !m_outputCache ||
        !liveSaved || m_stagingCache) {
        return result;
    }

    // The decode fill temporarily owns the staging cache in m_outputCache. Validate
    // that exact cache before copying, sanitizing, trimming, or swapping the live cache.
    if (!validatedOutputCommitPlayheadLocked(commit, m_outputCache.get()).has_value())
        return result;

    std::unique_ptr<OutputFrameCache> stagingSaved = std::move(m_outputCache);
    m_outputCache = std::make_unique<OutputFrameCache>(*liveSaved);
    m_stagingCache = std::make_unique<OutputFrameCache>(*stagingSaved);

#ifdef OLR_GPU_PIPELINE_BUILD
    if (sanitizeForDeviceLoss) {
        sanitizeCacheForDeviceLossLocked(m_outputCache.get());
        sanitizeCacheForDeviceLossLocked(m_stagingCache.get());
    }
#else
    Q_UNUSED(sanitizeForDeviceLoss);
#endif

    OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
    m_outputCache->trimWindow(keepFrom, keepTo, keepAudioFromSample, &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
    collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif

    result = commitOutputStateLocked(commit);
    if (result.committed) {
        liveSaved.reset();
        return result;
    }

    m_outputCache.reset();
    m_stagingCache.reset();
    m_outputCache = std::move(stagingSaved);
    return result;
}

#ifdef OLR_UNIT_TEST
void PlaybackWorker::setOutputCommitBarrierForTest(OutputCommitBarrierForTest* barrier) {
    m_outputCommitBarrierForTest = barrier;
}
#endif

OutputDispatchStats PlaybackWorker::outputStats() const {
    QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
    if (!m_outputRuntime) return OutputDispatchStats{};
#ifdef OLR_GPU_PIPELINE_BUILD
    drainGpuDeviceLossEvents();
    m_outputRuntime->recordGpuDeviceLossEvents(
        m_gpuDeviceLossEvents.load(std::memory_order_acquire));
    if (gpuPipelineEnabled()) {
        m_outputRuntime->recordGpuBudget(GpuBudget::instance().snapshot());
    } else {
        m_outputRuntime->recordGpuBudget(GpuBudgetSnapshot{});
    }
#endif
    return m_outputRuntime->stats();
}

void PlaybackWorker::refreshOutputAfterSeekCommit() {
    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return;
        runtime = m_outputRuntime.get();
        ++m_outputRuntimeImmediateDispatches;
    }

    runtime->dispatchImmediate();

    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        --m_outputRuntimeImmediateDispatches;
        m_outputRuntimeImmediateDispatchesIdle.wakeAll();
    }
}

void PlaybackWorker::refreshPreviewAfterSeekCommit() {
    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return;
        runtime = m_outputRuntime.get();
        ++m_outputRuntimeImmediateDispatches;
    }

    OutputDispatchRequest request;
    request.lane = OutputDispatchLane::PreviewFollower;
    runtime->dispatchImmediateWithReport(request);

    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        --m_outputRuntimeImmediateDispatches;
        m_outputRuntimeImmediateDispatchesIdle.wakeAll();
    }
}

OutputDispatchReport PlaybackWorker::dispatchPgmAfterSeekCommit(qint64 targetMs) {
    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return {};
        runtime = m_outputRuntime.get();
        ++m_outputRuntimeImmediateDispatches;
    }

    OutputDispatchRequest request;
    request.lane = OutputDispatchLane::PgmCritical;
    request.requiredBus = OutputBusId::pgm();
    request.requiredKind = OutputTargetKind::Ndi;
    request.requiredPlayheadMs = targetMs;
    request.requireNonPlaceholder = true;
    OutputDispatchReport report;
    for (int attempt = 0; attempt < 2; ++attempt) {
        report = runtime->dispatchImmediateWithReport(request);
        if (report.requiredSubmitted) break;
    }

    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        --m_outputRuntimeImmediateDispatches;
        m_outputRuntimeImmediateDispatchesIdle.wakeAll();
    }
    return report;
}

OutputDispatchReport PlaybackWorker::dispatchPgmCommitObligation(qint64 targetMs,
                                                                 uint64_t generation) {
    OutputDispatchReport report = dispatchPgmAfterSeekCommit(targetMs);
    if (report.requiredSubmitted) return report;

    bool confirmedEpochMismatch = false;
    for (const OutputSubmittedFrame& submittedFrame : report.submittedFrames) {
        if (submittedFrame.assignment.sourceBus == OutputBusId::pgm() &&
            submittedFrame.assignment.kind == OutputTargetKind::Ndi && submittedFrame.submitted &&
            !submittedFrame.identity.videoPlaceholder &&
            submittedFrame.identity.sampledPlayheadMs != targetMs) {
            confirmedEpochMismatch = true;
            break;
        }
    }
    if (!confirmedEpochMismatch) return report;

    // A scheduled tick can advance the freshly-reset epoch before the critical
    // dispatch runs. A successfully submitted non-placeholder frame at the wrong
    // sampled playhead positively identifies that drift. Sink rejection, a missing
    // endpoint, or ordinary submission failure cannot enter this recommit path.
    OutputCommitResult retryCommit;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_operatorSeekCompletion.waiting || m_operatorSeekCompletion.completed ||
            m_operatorSeekCompletion.generation != generation ||
            m_operatorSeekCompletion.targetMs != targetMs) {
            return report;
        }

        QMutexLocker bufferLocker(&m_bufferMutex);
        uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
        OutputCommit commit;
        commit.playheadMs = targetMs;
        commit.seekGeneration = generation;
        commit.gpuGeneration = gpuGeneration;
        commit.coverageMode = OutputCoverageMode::OperatorSeek;
        commit.dispatch = PostCommitDispatch::PgmCritical;
        retryCommit = commitOutputStateLocked(commit);
    }

    if (retryCommit.committed && retryCommit.dispatch == PostCommitDispatch::PgmCritical)
        report = dispatchPgmAfterSeekCommit(retryCommit.committedPlayheadMs);
    return report;
}

void PlaybackWorker::completeOperatorSeekTransaction(uint64_t generation, qint64 targetMs,
                                                     const OutputDispatchReport& report) {
    QMutexLocker locker(&m_mutex);
    if (!m_operatorSeekCompletion.waiting || m_operatorSeekCompletion.generation != generation ||
        m_operatorSeekCompletion.targetMs != targetMs) {
        return;
    }
    m_operatorSeekCompletion.completed = true;
    m_operatorSeekCompletion.submittedPgm = report.requiredSubmitted;
    if (report.requiredSubmitted) {
        m_operatorSeekCompletion.pgmIdentity = report.requiredIdentity;
        m_operatorSeekCompletion.message = QStringLiteral("PGM submitted");
    } else {
        m_operatorSeekCompletion.message = QStringLiteral("PGM output was not submitted");
    }
    m_operatorSeekCondition.wakeAll();

    OperatorSeekResult emitted;
    emitted.completed = report.requiredSubmitted;
    emitted.submittedPgm = report.requiredSubmitted;
    emitted.targetMs = targetMs;
    emitted.generation = generation;
    emitted.pgmIdentity = m_operatorSeekCompletion.pgmIdentity;
    emitted.message = m_operatorSeekCompletion.message;
    emit operatorSeekCompleted(generation, emitted);
}

bool PlaybackWorker::hasOperatorSeekTransaction(uint64_t generation) {
    QMutexLocker locker(&m_mutex);
    return m_operatorSeekCompletion.waiting && m_operatorSeekCompletion.generation == generation &&
           !m_operatorSeekCompletion.completed;
}

void PlaybackWorker::abandonOperatorSeekTransaction(quint64 generation) {
    QMutexLocker locker(&m_mutex);
    if (m_operatorSeekCompletion.generation == generation && m_operatorSeekCompletion.waiting &&
        !m_operatorSeekCompletion.completed) {
        m_operatorSeekCompletion.waiting = false;
    }
}

bool PlaybackWorker::tryCompleteOperatorSeekFromCurrentOutputCache(qint64 targetMs,
                                                                   uint64_t generation) {
    OutputCommitResult outputCommit;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_operatorSeekCompletion.waiting ||
            m_operatorSeekCompletion.generation != generation ||
            m_operatorSeekCompletion.targetMs != targetMs || m_operatorSeekCompletion.completed ||
            m_operatorSeekCompletion.pgmDispatchAttempted) {
            return false;
        }
        if (!CommitGate::canCommitReposition(generation,
                                             m_seekGeneration.load(std::memory_order_acquire),
                                             m_seekTargetMs >= 0)) {
            return false;
        }

        QMutexLocker bufferLocker(&m_bufferMutex);
        uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
        OutputCommit commit;
        commit.playheadMs = targetMs;
        commit.seekGeneration = generation;
        commit.gpuGeneration = gpuGeneration;
        commit.cacheAction = OutputCacheAction::Publish;
        commit.coverageMode = OutputCoverageMode::OperatorSeek;
        commit.dispatch = PostCommitDispatch::PgmCritical;
        outputCommit = commitOutputStateLocked(commit);
    }

    if (!outputCommit.committed) return false;

#ifdef OLR_UNIT_TEST
    if (m_outputCommitBarrierForTest) m_outputCommitBarrierForTest->enterAndWait();
#endif

    const OutputDispatchReport pgmReport =
        outputCommit.dispatch == PostCommitDispatch::PgmCritical
            ? dispatchPgmCommitObligation(outputCommit.committedPlayheadMs, generation)
            : OutputDispatchReport{};
    if (pgmReport.requiredSubmitted)
        completeOperatorSeekTransaction(generation, outputCommit.committedPlayheadMs, pgmReport);
    return pgmReport.requiredSubmitted;
}

void PlaybackWorker::maybeCompleteOperatorSeekAfterDecodedPacket(qint64 targetMs,
                                                                 uint64_t generation,
                                                                 bool& operatorPgmCompletedEarly) {
    if (!operatorPgmCompletedEarly) {
        operatorPgmCompletedEarly =
            tryCompleteOperatorSeekFromCurrentOutputCache(targetMs, generation);
    }
}

bool PlaybackWorker::allowDisplayableFallbackForReposition(uint64_t generation) {
    return !hasOperatorSeekTransaction(generation);
}

PlaybackWorker::PlaybackCounters PlaybackWorker::counters() const {
    PlaybackCounters counters = m_counters;
    counters.transportPlayheadMs = m_transport ? m_transport->currentPos() : 0;
    counters.committedPlayheadMs = m_committedPlayheadMs.load(std::memory_order_acquire);
    counters.lastVisiblePlayheadMs = m_lastVisiblePlayheadMs.load(std::memory_order_acquire);
    counters.seekGeneration = m_seekGeneration.load(std::memory_order_acquire);
    counters.committedGeneration = m_committedGeneration.load(std::memory_order_acquire);
    counters.outputPlayheadCacheGuarded =
        m_outputPlayheadCacheGuarded.load(std::memory_order_acquire);
#ifdef OLR_GPU_PIPELINE_BUILD
    counters.gpuReadToCpuCount = gpuFrameReadToCpuCount();
    counters.committedGpuGeneration = m_committedGpuGeneration.load(std::memory_order_acquire);
    counters.currentGpuGeneration = GpuGenerationCounter::instance().current();
    counters.forceLiveOutputSnapshots = m_forceLiveOutputSnapshots.load(std::memory_order_acquire);
    counters.memoryPressureLatched = m_memoryPressureLatched.load(std::memory_order_acquire);
    counters.gpuPipelineState = m_gpuPipelineState.load(std::memory_order_acquire);
#endif
    return counters;
}

uint64_t PlaybackWorker::gpuGeneration() const {
#ifdef OLR_GPU_PIPELINE_BUILD
    return GpuGenerationCounter::instance().current();
#else
    return 0;
#endif
}

#ifdef OLR_GPU_PIPELINE_BUILD
void PlaybackWorker::injectGpuDeviceLossForTest() {
    m_injectGpuDeviceLossForTest.store(true, std::memory_order_release);
}
#endif

void PlaybackWorker::stop() {
    // The interrupt callback (registered on m_fmtCtx) aborts any blocking
    // av_read_frame.  Do NOT poke m_fmtCtx->pb from this thread: the
    // worker owns that object, and writes raced with the EOF handler.
    m_running = false;
    requestInterruption();
    m_workerWake.wakeAll();
    m_operatorSeekCondition.wakeAll();
    if (QThread::currentThread() != this) {
        wait();
    }
}

int PlaybackWorker::ffmpegInterruptCallback(void* opaque) {
    PlaybackWorker* worker = static_cast<PlaybackWorker*>(opaque);
    return worker ? worker->shouldInterruptFfmpeg() : 0;
}

bool PlaybackWorker::shouldInterrupt() const {
    return !m_running || isInterruptionRequested();
}

bool PlaybackWorker::shouldInterruptFfmpeg() const {
    return liveReadDeadlineInterruptsFromProbe(
        shouldInterrupt(), m_liveReadDeadlineSteadyMs.load(std::memory_order_acquire),
        steadyClockMs());
}

bool PlaybackWorker::liveReadDeadlineExpired(int64_t nowMs) const {
    return liveReadDeadlineInterruptsFromProbe(
        /*baseInterrupt=*/false, m_liveReadDeadlineSteadyMs.load(std::memory_order_acquire), nowMs);
}

int PlaybackWorker::readPrimaryFrame(AVPacket* pkt) {
    if (!m_fmtCtx) return AVERROR_EOF;

    const int64_t deadlineMs = steadyClockMs() + kLiveReadTimeoutMs;
    m_liveReadDeadlineSteadyMs.store(deadlineMs, std::memory_order_release);
    const int ret = av_read_frame(m_fmtCtx, pkt);
    const bool timedOut = !shouldInterrupt() && ret < 0 && liveReadDeadlineExpired(steadyClockMs());
    m_liveReadDeadlineSteadyMs.store(-1, std::memory_order_release);
    if (timedOut) {
        return AVERROR_EOF;
    }
    return ret;
}

void PlaybackWorker::emitTelemetry(int64_t P, int64_t newest, double speed) {
    if (!qEnvironmentVariableIsSet("OLR_PB_TELEMETRY")) return;
    fprintf(stderr,
            "SEC repos=%d reuse=%d revseek=%d eof=%d skip=%d apush=%d drop=%d P=%lld newest=%lld "
            "spd=%.2f\n",
            m_counters.reposition, m_counters.reuseSeek, m_counters.reverseChunkSeek,
            m_counters.eofTailSeek, m_counters.skipForward, m_counters.audioPushes,
            m_counters.framesDropped, (long long) P, (long long) newest, speed);

    if (!qEnvironmentVariableIsSet("OLR_PB_TELEMETRY_DETAIL")) return;

    auto describeFrame = [](const FrameHandle& handle) {
        if (handle.isNull()) return QStringLiteral("null");
        const FrameMetadata meta = handle.metadata();
        QString sample = QStringLiteral("noCpu");
        const MediaVideoFrameView view(handle);
        if (view.isValid()) {
            const int y = uchar(view.planeY.at(0));
            const int u = uchar(view.planeU.at(0));
            const int v = uchar(view.planeV.at(0));
            sample = QStringLiteral("yuv=%1,%2,%3").arg(y).arg(u).arg(v);
        }
        return QStringLiteral("feed=%1 pts=%2 ph=%3 gpu=%4 gen=%5 seq=%6 fmt=%7 %8")
            .arg(meta.key.feedIndex)
            .arg(meta.key.ptsMs)
            .arg(meta.key.isPlaceholder ? 1 : 0)
            .arg(handle.isGpuBacked() ? 1 : 0)
            .arg(qulonglong(meta.gpuGeneration))
            .arg(meta.decodedSequence)
            .arg(int(meta.key.format))
            .arg(sample);
    };

    struct FeedLine {
        int feed = -1;
        qint64 oldest = -1;
        qint64 newest = -1;
        qint64 delivered = -1;
        int count = 0;
        QString latest;
        QString cacheAt;
        QString cacheNext;
    };

    QVector<FeedLine> feeds;
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
#ifdef OLR_GPU_PIPELINE_BUILD
        const uint64_t gpuGeneration = GpuGenerationCounter::instance().current();
#else
        const uint64_t gpuGeneration = 0;
#endif
        feeds.reserve(m_decoderBank.size());
        for (const DecoderTrack* track : m_decoderBank) {
            if (!track) continue;
            FeedLine line;
            line.feed = track->feedIndex;
            line.oldest = track->buffer.oldestPts();
            line.newest = track->buffer.newestPts();
            line.delivered = track->lastDeliveredPtsMs;
            line.count = track->buffer.size();
            const QVector<TrackBuffer::Frame> frames = track->buffer.framesSnapshot();
            line.latest = frames.isEmpty() ? QStringLiteral("empty")
                                           : describeFrame(frames.constLast().frame);
            if (m_outputCache) {
                const std::optional<FrameHandle> at =
                    m_outputCache->videoFrameAtFreshForGeneration(line.feed, P, gpuGeneration);
                const std::optional<FrameHandle> next =
                    m_outputCache->firstFreshVideoFrameAtOrAfter(line.feed, P, gpuGeneration);
                line.cacheAt = at.has_value() ? describeFrame(*at) : QStringLiteral("none");
                line.cacheNext = next.has_value() ? describeFrame(*next) : QStringLiteral("none");
            } else {
                line.cacheAt = QStringLiteral("noOutputCache");
                line.cacheNext = QStringLiteral("noOutputCache");
            }
            feeds.append(line);
        }
    }

    for (const FeedLine& line : feeds) {
        fprintf(stderr,
                "FEED feed=%d P=%lld bufOld=%lld bufNew=%lld bufN=%d delivered=%lld "
                "latest{%s} cacheAt{%s} cacheNext{%s}\n",
                line.feed, (long long) P, (long long) line.oldest, (long long) line.newest,
                line.count, (long long) line.delivered, qPrintable(line.latest),
                qPrintable(line.cacheAt), qPrintable(line.cacheNext));
    }

    const OutputDispatchStats stats = outputStats();
    for (auto it = stats.targets.constBegin(); it != stats.targets.constEnd(); ++it) {
        const OutputTargetDispatchStats& target = it.value();
        if (!target.hasLastIdentity) {
            fprintf(stderr, "OUT target=%s frames=%lld placeholders=%lld held=%lld noIdentity\n",
                    qPrintable(it.key()), (long long) target.framesSubmitted,
                    (long long) target.placeholderFrames, (long long) stats.heldFrames);
            continue;
        }
        const OutputFrameIdentity& id = target.lastIdentity;
        fprintf(stderr,
                "OUT target=%s frames=%lld placeholders=%lld repeated=%lld held=%lld bus=%d:%d "
                "out=%lld sampled=%lld srcFeed=%d srcPts=%lld ph=%d gpuGen=%llu seq=%lld "
                "vhash=%u\n",
                qPrintable(it.key()), (long long) target.framesSubmitted,
                (long long) target.placeholderFrames, (long long) target.repeatedPayloadFrames,
                (long long) stats.heldFrames, int(id.bus.kind), id.bus.index,
                (long long) id.outputFrameIndex, (long long) id.sampledPlayheadMs,
                id.sourceFeedIndex, (long long) id.sourcePtsMs, id.videoPlaceholder ? 1 : 0,
                (unsigned long long) id.videoGpuGeneration, (long long) id.sourceDecodedSequence,
                id.videoHash);
    }
}

// ---------------------------------------------------------------------------
// Scheduler helpers (spec §3 symbols / §6).
//
// These are added for the windowed scheduler that lands in Task 5. The
// existing (old) playback loop does NOT call them yet — behavior is unchanged
// this task. They are implemented correctly now (pure/simple) except
// repositionTo, which is stubbed until Task 5.
// ---------------------------------------------------------------------------
int PlaybackWorker::fps() const {
    return qMax(1, m_transport->fps());
}

int64_t PlaybackWorker::frameDurMs() const {
    return 1000 / fps(); // fps() >= 1 so no divide-by-zero
}

int64_t PlaybackWorker::maxPriorCoverageMs() const {
    return qMax<int64_t>(frameDurMs(), windowLeadMs() + windowChunkMs() + windowSlackMs());
}

std::optional<qint64> PlaybackWorker::outputFeedCoverageInCache(const OutputFrameCache& cache,
                                                                int feedIndex, int64_t playheadMs,
                                                                uint64_t gpuGeneration,
                                                                OutputCoverageMode mode) const {
    if (feedIndex < 0 || feedIndex >= cache.feedCount()) return std::nullopt;

    const std::optional<FrameHandle> rawAt = cache.videoFrameAt(feedIndex, playheadMs);
    if (rawAt.has_value() && rawAt->metadata().key.ptsMs == playheadMs) {
        if (rawAt->metadata().key.isPlaceholder || rawAt->isStaleForGeneration(gpuGeneration))
            return std::nullopt;
        return playheadMs;
    }

    const std::optional<FrameHandle> prior =
        cache.videoFrameAtFreshForGeneration(feedIndex, playheadMs, gpuGeneration);
    if (prior.has_value() && !prior->metadata().key.isPlaceholder) {
        const qint64 ageMs = playheadMs - prior->metadata().key.ptsMs;
        if (ageMs == 0 || (ageMs > 0 && ageMs < frameDurMs())) return playheadMs;
    }

    const std::optional<FrameHandle> future =
        cache.firstFreshVideoFrameAtOrAfter(feedIndex, playheadMs, gpuGeneration);
    if (!future.has_value() || future->metadata().key.isPlaceholder) return std::nullopt;

    const qint64 futureDeltaMs = future->metadata().key.ptsMs - playheadMs;
    if (futureDeltaMs < 0) return std::nullopt;
    if (prior.has_value()) {
        if (OutputFrameSelection::isTimestampRoundingFuture(futureDeltaMs))
            return std::optional<qint64>(playheadMs);
        if (mode == OutputCoverageMode::OperatorSeek) return std::nullopt;
        const qint64 priorAgeMs = playheadMs - prior->metadata().key.ptsMs;
        const qint64 nearFutureMs =
            qMax<qint64>(1, frameDurMs() + OutputFrameSelection::kTimestampRoundingToleranceMs);
        const qint64 maxLowerCadenceHoldMs = qMax<qint64>(200, frameDurMs() * 6);
        if (futureDeltaMs <= nearFutureMs && priorAgeMs > maxLowerCadenceHoldMs)
            return std::nullopt;
        // A near future frame within one output tick means the same-cadence target frame is
        // missing; bracketed lower-cadence sources have a wider gap and intentionally hold prior.
        if (mode == OutputCoverageMode::StrictSeek && futureDeltaMs <= frameDurMs())
            return std::nullopt;
        return futureDeltaMs <= maxPriorCoverageMs() ? std::optional<qint64>(playheadMs)
                                                     : std::nullopt;
    }
    if (OutputFrameSelection::isTimestampRoundingFuture(futureDeltaMs))
        return std::optional<qint64>(future->metadata().key.ptsMs);
    if (mode == OutputCoverageMode::Displayable && futureDeltaMs <= qMax<qint64>(1, frameDurMs()))
        return std::optional<qint64>(future->metadata().key.ptsMs);
    return std::nullopt;
}

bool PlaybackWorker::outputFeedCoversPlayheadLocked(int feedIndex, int64_t playheadMs,
                                                    uint64_t gpuGeneration,
                                                    OutputCoverageMode mode) const {
    if (!m_outputCache) return false;
    return outputFeedCoverageInCache(*m_outputCache, feedIndex, playheadMs, gpuGeneration, mode)
        .has_value();
}

bool PlaybackWorker::outputCacheCoversPlayheadInCacheLocked(const OutputFrameCache& cache,
                                                            int64_t playheadMs,
                                                            uint64_t gpuGeneration,
                                                            OutputCoverageMode mode) const {
    if (m_outputFeedCount <= 0) return false;

    const bool requireAllFeeds = m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire);
    if (requireAllFeeds) {
        for (int feed = 0; feed < m_outputFeedCount; ++feed) {
            if (!outputFeedCoverageInCache(cache, feed, playheadMs, gpuGeneration, mode)
                     .has_value())
                return false;
        }
        return true;
    }

    int selected = m_selectedOutputFeed.load(std::memory_order_relaxed);
    if (selected < 0 && m_outputFeedCount > 0) selected = 0;
    return outputFeedCoverageInCache(cache, selected, playheadMs, gpuGeneration, mode).has_value();
}

bool PlaybackWorker::outputCacheCoversPlayheadLocked(int64_t playheadMs, uint64_t gpuGeneration,
                                                     OutputCoverageMode mode) const {
    return m_outputCache &&
           outputCacheCoversPlayheadInCacheLocked(*m_outputCache, playheadMs, gpuGeneration, mode);
}

std::optional<qint64> PlaybackWorker::outputCacheDisplayablePlayheadInCacheLocked(
    const OutputFrameCache& cache, qint64 playheadMs, uint64_t gpuGeneration) const {
    if (m_outputFeedCount <= 0) return std::nullopt;

    auto candidateForFeed = [&](int feedIndex) -> std::optional<qint64> {
        if (const std::optional<qint64> covered = outputFeedCoverageInCache(
                cache, feedIndex, playheadMs, gpuGeneration, OutputCoverageMode::Displayable)) {
            return covered;
        }
        const std::optional<FrameHandle> future =
            cache.firstFreshVideoFrameAtOrAfter(feedIndex, playheadMs, gpuGeneration);
        if (!future.has_value() || future->metadata().key.isPlaceholder) {
            const std::optional<FrameHandle> prior =
                cache.videoFrameAtFreshForGeneration(feedIndex, playheadMs, gpuGeneration);
            if (!prior.has_value() || prior->metadata().key.isPlaceholder) return std::nullopt;
            const qint64 ageMs = playheadMs - prior->metadata().key.ptsMs;
            if (ageMs >= 0 && ageMs <= maxPriorCoverageMs()) return playheadMs;
            return std::nullopt;
        }
        return future->metadata().key.ptsMs;
    };

    qint64 candidate = std::numeric_limits<qint64>::min();
    const bool requireAllFeeds = m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire);
    if (requireAllFeeds) {
        for (int feed = 0; feed < m_outputFeedCount; ++feed) {
            const std::optional<qint64> feedCandidate = candidateForFeed(feed);
            if (!feedCandidate.has_value()) return std::nullopt;
            candidate = qMax(candidate, *feedCandidate);
        }
    } else {
        int selected = m_selectedOutputFeed.load(std::memory_order_relaxed);
        if (selected < 0 && m_outputFeedCount > 0) selected = 0;
        const std::optional<qint64> feedCandidate = candidateForFeed(selected);
        if (!feedCandidate.has_value()) return std::nullopt;
        candidate = *feedCandidate;
    }

    if (candidate == std::numeric_limits<qint64>::min()) return std::nullopt;
    auto hasFrameAtCandidate = [&](int feedIndex) {
        const std::optional<FrameHandle> frame =
            cache.videoFrameAtFreshForGeneration(feedIndex, candidate, gpuGeneration);
        return frame.has_value() && !frame->metadata().key.isPlaceholder;
    };
    if (requireAllFeeds) {
        for (int feed = 0; feed < m_outputFeedCount; ++feed) {
            if (!hasFrameAtCandidate(feed)) return std::nullopt;
        }
    } else {
        int selected = m_selectedOutputFeed.load(std::memory_order_relaxed);
        if (selected < 0 && m_outputFeedCount > 0) selected = 0;
        if (!hasFrameAtCandidate(selected)) return std::nullopt;
    }
    return candidate;
}

std::optional<qint64>
PlaybackWorker::outputCacheDisplayablePlayheadLocked(qint64 playheadMs,
                                                     uint64_t gpuGeneration) const {
    if (!m_outputCache) return std::nullopt;
    return outputCacheDisplayablePlayheadInCacheLocked(*m_outputCache, playheadMs, gpuGeneration);
}

bool PlaybackWorker::outputCacheCoversPlayhead(int64_t playheadMs) const {
    QMutexLocker bufferLocker(&m_bufferMutex);

    uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
    return outputCacheCoversPlayheadLocked(playheadMs, gpuGeneration,
                                           OutputCoverageMode::OperatorSeek);
}

bool PlaybackWorker::pausedPlayheadNeedsWork(int64_t playheadMs) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_seekTargetMs >= 0) return true;
    }

    QMutexLocker bufferLocker(&m_bufferMutex);
    if (m_decoderBank.isEmpty()) return true;

    FrameHandle frame;
    int64_t ptsMs = -1;
    DecoderTrack* ref = m_decoderBank[0];
    if (!ref || !ref->buffer.frameAt(playheadMs, frame, ptsMs) || ptsMs != ref->lastDeliveredPtsMs)
        return true;

    if (!m_outputCache || m_outputFeedCount <= 0) return false;

    uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
    const bool requireAllFeeds = m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire);
    if (requireAllFeeds) {
        for (int feed = 0; feed < m_outputFeedCount; ++feed) {
            if (!outputFeedCoversPlayheadLocked(feed, playheadMs, gpuGeneration)) return true;
        }
        return false;
    }

    int selected = m_selectedOutputFeed.load(std::memory_order_relaxed);
    if (selected < 0 && m_outputFeedCount > 0) selected = 0;
    if (!outputFeedCoversPlayheadLocked(selected, playheadMs, gpuGeneration)) return true;

    return false;
}

int64_t PlaybackWorker::windowLeadMs() const {
    return qMax(1, m_residencyWindowParams.leadMs);
}

int64_t PlaybackWorker::windowTrailMs() const {
    return qMax(0, m_residencyWindowParams.trailMs);
}

int64_t PlaybackWorker::windowChunkMs() const {
    return qMax(1, m_residencyWindowParams.chunkMs);
}

int64_t PlaybackWorker::windowSlackMs() const {
    return qMax(0, m_residencyWindowParams.slackMs);
}

int64_t PlaybackWorker::windowAudioTrailMs() const {
    return qMax(0, m_residencyWindowParams.audioTrailMs);
}

#ifdef OLR_UNIT_TEST
void PlaybackWorker::setResidencyWindowParamsForTest(const ResidencyWindowParams& params) {
    m_residencyWindowParams = params;
}

int64_t PlaybackWorker::liveGrowthFileSizeForTest(int64_t avioSize, const QString& filePath) {
    return liveGrowthFileSizeFromProbe(avioSize, filePath);
}

int64_t PlaybackWorker::liveEofRecoveryAnchorMsForTest(int64_t playheadMs,
                                                       int64_t newestBeforeEofMs, int64_t trailMs,
                                                       int64_t frameDurationMs) {
    return liveEofRecoveryAnchorMs(playheadMs, newestBeforeEofMs, trailMs, frameDurationMs);
}

bool PlaybackWorker::liveReadDeadlineInterruptsForTest(bool baseInterrupt, int64_t deadlineMs,
                                                       int64_t nowMs) {
    return liveReadDeadlineInterruptsFromProbe(baseInterrupt, deadlineMs, nowMs);
}

#ifdef OLR_GPU_PIPELINE_BUILD
void PlaybackWorker::evaluateGpuMemoryPressureForTest(uint64_t availableBytes, bool memoryWarning,
                                                      qint64 nowMs) {
    evaluateGpuMemoryPressure(availableBytes, memoryWarning, nowMs);
}
#endif
#endif

int64_t PlaybackWorker::liveGrowthFileSize() const {
    const int64_t avioSize = (m_fmtCtx && m_fmtCtx->pb) ? avio_size(m_fmtCtx->pb) : -1;
    return liveGrowthFileSizeFromProbe(avioSize, m_currentFilePath);
}

int PlaybackWorker::capFrames(int trackCount) const {
    // capFrames = clamp( ceil(windowMs / frameDurMs) + 4, 12,
    //                    max(12, kGlobalFrameBudget / max(1,trackCount)) )
    const int64_t windowMs =
        windowLeadMs() + windowChunkMs() + windowTrailMs() + 2 * windowSlackMs();
    const int64_t dur = qMax<int64_t>(1, frameDurMs());    // never divide by zero
    const int64_t ceilFrames = (windowMs + dur - 1) / dur; // integer ceil
    int64_t want = ceilFrames + 4;

    const int tc = qMax(1, trackCount);
    int64_t hi = qMax<int64_t>(12, qMax(1, m_residencyWindowParams.globalFrameBudget) / tc);
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled() && gpuPipelineState() == GpuPipelineState::Gpu &&
        !m_memoryPressureLatched.load(std::memory_order_acquire)) {
        hi = gpuPerTrackWindowCap(tc, int(hi));
    }
#endif
    if (m_residencyWindowParams.perTrackCapOverride > 0)
        hi = m_residencyWindowParams.perTrackCapOverride;
    const int64_t lo = qMin<int64_t>(12, hi);

    if (want < lo) want = lo;
    if (want > hi) want = hi;
    return int(want);
}

int64_t PlaybackWorker::newestPtsMin() const {
    // min-newest across non-empty video tracks, EXCLUDING tracks whose newest
    // PTS lags the cross-track max-newest by more than kLeadMs ("stalled,
    // will-backfill" tracks per the non-interleaved muxer).
    QMutexLocker bufferLocker(&m_bufferMutex);
    int64_t maxNewest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n > maxNewest) maxNewest = n;
    }
    if (maxNewest < 0) return -1; // all tracks empty

    int64_t minNewest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n < 0) continue;                          // empty track
        if (n < maxNewest - windowLeadMs()) continue; // stalled track — exclude
        if (minNewest < 0 || n < minNewest) minNewest = n;
    }
    return minNewest;
}

int64_t PlaybackWorker::oldestPtsMin() const {
    // min-oldest across non-empty video tracks, EXCLUDING stalled tracks
    // (same staleness rule as newestPtsMin, keyed off cross-track max-newest).
    QMutexLocker bufferLocker(&m_bufferMutex);
    int64_t maxNewest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n > maxNewest) maxNewest = n;
    }
    if (maxNewest < 0) return -1; // all tracks empty

    int64_t minOldest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n < 0) continue;                          // empty track
        if (n < maxNewest - windowLeadMs()) continue; // stalled track — exclude
        const int64_t o = track->buffer.oldestPts();
        if (minOldest < 0 || o < minOldest) minOldest = o;
    }
    return minOldest;
}

int64_t PlaybackWorker::newestPtsMax() const {
    // plain cross-track max of newestPts, ignoring empty tracks (pts < 0).
    QMutexLocker bufferLocker(&m_bufferMutex);
    int64_t maxNewest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n > maxNewest) maxNewest = n;
    }
    return maxNewest;
}

int64_t PlaybackWorker::refNewestPts() const {
    QMutexLocker bufferLocker(&m_bufferMutex);
    if (m_decoderBank.isEmpty()) return -1;
    return m_decoderBank[0]->buffer.newestPts();
}

int64_t PlaybackWorker::refOldestPts() const {
    QMutexLocker bufferLocker(&m_bufferMutex);
    if (m_decoderBank.isEmpty()) return -1;
    return m_decoderBank[0]->buffer.oldestPts();
}

void PlaybackWorker::resetDedup() {
    QMutexLocker bufferLocker(&m_bufferMutex);
    for (auto* track : m_decoderBank)
        track->lastDeliveredPtsMs = -1;
}

void PlaybackWorker::clearDecoderBuffers(bool invalidateGpuGeneration) {
#ifndef OLR_GPU_PIPELINE_BUILD
    Q_UNUSED(invalidateGpuGeneration);
#endif
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
#ifdef OLR_GPU_PIPELINE_BUILD
        if (invalidateGpuGeneration && gpuPipelineEnabled())
            GpuGenerationCounter::instance().bump();
#endif
        for (auto* track : m_decoderBank) {
            TrackBuffer::EvictedFrames evicted;
            track->buffer.clear(&evicted);
#ifdef OLR_GPU_PIPELINE_BUILD
            collectEvictedGpuFramesLocked(evicted);
#endif
            track->decimateCounter = 0;
        }
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    drainEvictedGpuFrames();
#endif
    // NOTE: deliberately does NOT clear m_outputCache. The OutputRuntime paints
    // exclusively from m_outputCache; wiping it here makes the next ~1ms tick
    // snapshot an empty cache and render the gray placeholder (the seek flash).
    // The cache's stale frames are harmless: the forward fill re-inserts the
    // new frames before the playhead reaches them, and the regular cache-window
    // trim drops stale frames. See docs/superpowers/plans (Tier 1 Task 1).
}

bool PlaybackWorker::resyncPrimaryDecodeCursorTo(qint64 targetMs) {
    if (!m_fmtCtx || m_decoderBank.isEmpty()) return false;

    const int primaryVideoStreamIndex = m_decoderBank[0]->streamIndex;
    if (primaryVideoStreamIndex < 0 ||
        primaryVideoStreamIndex >= static_cast<int>(m_fmtCtx->nb_streams)) {
        return false;
    }

    AVStream* vStream = m_fmtCtx->streams[primaryVideoStreamIndex];
    const qint64 anchor = qMax<qint64>(0, targetMs - windowTrailMs());

    if (m_fmtCtx->pb) {
        m_fmtCtx->pb->eof_reached = 0;
        m_fmtCtx->pb->error = 0;
    }
    avformat_flush(m_fmtCtx);
    const int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
    int seekRet = av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
    if (seekRet < 0) {
        const AVRational avTimeBase{1, AV_TIME_BASE};
        const int64_t fileSeekPts = av_rescale_q(anchor, {1, 1000}, avTimeBase);
        seekRet = avformat_seek_file(m_fmtCtx, -1, INT64_MIN, fileSeekPts, fileSeekPts,
                                     AVSEEK_FLAG_BACKWARD);
    }
    if (seekRet < 0) {
        qWarning() << "PlaybackWorker: primary decode cursor resync failed"
                   << "targetMs" << targetMs << "anchorMs" << anchor << "ret" << seekRet;
        return false;
    }
    avformat_flush(m_fmtCtx);

    clearDecoderBuffers(/*invalidateGpuGeneration*/ false);
    m_reverseAnchorMs = INT64_MAX;
    m_audioQueue.clear();
    for (auto* aTrack : m_audioDecoderBank) {
        aTrack->lastEnqueuedPtsMs = -1;
        aTrack->lastCachedPtsMs = -1;
        if (aTrack->codecCtx) avcodec_flush_buffers(aTrack->codecCtx);
    }
    for (auto* track : m_decoderBank) {
        if (track->codecCtx) avcodec_flush_buffers(track->codecCtx);
        if (track->nativeDecoder) track->nativeDecoder->reset();
    }
    if (m_audioPlayer) m_audioPlayer->clear();
    m_counters.skipForward++;
    return true;
}

bool PlaybackWorker::reuseAt(int64_t target) {
    // True iff the bank and output cache already cover an operator seek target.
    // Unlike preview displayability, this path must not certify broad lower-cadence
    // holds: a seek/step transaction is allowed to reuse exact frames or the
    // immediately preceding source frame within one output tick only.
    QMutexLocker bufferLocker(&m_bufferMutex);
    if (m_decoderBank.isEmpty()) return false;

    auto trackCoversTarget = [&](DecoderTrack* track) {
        FrameHandle frame;
        int64_t pts = -1;
        if (!track || !track->buffer.frameAt(target, frame, pts) ||
            frame.metadata().key.isPlaceholder)
            return false;
        const qint64 ageMs = target - pts;
        if (ageMs != 0 && ageMs >= frameDurMs()) return false;
        return true;
    };

    if (m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire)) {
        for (auto* track : m_decoderBank) {
            if (!trackCoversTarget(track)) return false;
        }
    } else {
        int selected = m_selectedOutputFeed.load(std::memory_order_relaxed);
        if (selected < 0) selected = 0;
        DecoderTrack* selectedTrack = nullptr;
        for (auto* track : m_decoderBank) {
            if (track && track->feedIndex == selected) {
                selectedTrack = track;
                break;
            }
        }
        if (!trackCoversTarget(selectedTrack)) return false;
    }

    if (m_outputCache && m_outputFeedCount > 0) {
        uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
        if (m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire)) {
            for (auto* track : m_decoderBank) {
                if (!track || track->feedIndex < 0 || track->feedIndex >= m_outputFeedCount)
                    return false;
                if (!outputFeedCoversPlayheadLocked(track->feedIndex, target, gpuGeneration,
                                                    OutputCoverageMode::OperatorSeek))
                    return false;
            }
        } else {
            int selected = m_selectedOutputFeed.load(std::memory_order_relaxed);
            if (selected < 0) selected = 0;
            if (!outputFeedCoversPlayheadLocked(selected, target, gpuGeneration,
                                                OutputCoverageMode::OperatorSeek))
                return false;
        }
    }
    return true;
}

#ifdef OLR_GPU_PIPELINE_BUILD
PlaybackWorker::GpuPipelineState PlaybackWorker::gpuPipelineState() const {
    return static_cast<GpuPipelineState>(m_gpuPipelineState.load(std::memory_order_acquire));
}

bool PlaybackWorker::gpuPathActive() const {
    const auto gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
    return gpuPipelineState() == GpuPipelineState::Gpu && gpuRhi && gpuRhi->isValid() &&
           !gpuRhi->deviceLost() && !GpuDeviceLossMonitor::instance().isLost() &&
           !gpuLifecycleSuspended() && !m_memoryPressureLatched.load(std::memory_order_acquire);
}

bool PlaybackWorker::gpuLifecycleSuspended() const {
    const IosGpuLifecycleSink* sink = iosGpuLifecycleSink();
    return sink && sink->isSuspended();
}

bool PlaybackWorker::gpuDeviceLossPending() const {
    // Backend detection sites publish the process-wide latch. Output-thread
    // snapshots must not dereference smart pointers that the worker replaces
    // during recovery.
    return GpuDeviceLossMonitor::instance().isLost();
}

bool PlaybackWorker::consumeGpuDeviceLossRebuildBudget() {
    int remaining = m_gpuDeviceLossRebuildsRemaining.load(std::memory_order_acquire);
    while (remaining > 0) {
        if (m_gpuDeviceLossRebuildsRemaining.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void PlaybackWorker::drainGpuDeviceLossEvents() const {
    const uint64_t observed = GpuDeviceLossMonitor::instance().lossCount();
    uint64_t previous = m_gpuLastObservedLossCount.load(std::memory_order_acquire);
    while (observed > previous) {
        if (m_gpuLastObservedLossCount.compare_exchange_weak(
                previous, observed, std::memory_order_acq_rel, std::memory_order_acquire)) {
            m_gpuDeviceLossEvents.fetch_add(qint64(observed - previous), std::memory_order_acq_rel);
            return;
        }
    }
    if (observed < previous) m_gpuLastObservedLossCount.store(observed, std::memory_order_release);
}

void PlaybackWorker::cleanupGpuRetirementsForDeviceLoss(bool allowTokenlessTestGate,
                                                        bool pollBackends) {
    constexpr int kDeviceLossReadbackDrainMs = 100;
    if (pollBackends) {
        const auto gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
        if (gpuRhi) (void) gpuRhi->pollDeviceLoss();
#ifdef _WIN32
        if (m_winGpuImportEdge) (void) m_winGpuImportEdge->deviceLost();
#endif
    }
    auto& lossMonitor = GpuDeviceLossMonitor::instance();
    GpuRetireRegistry registry;
    GpuValidatedLossResult recovery =
        lossMonitor.withValidatedDeadDomains([&](const GpuValidatedDeadDomains& deadDomains) {
            return registry.abandonAllNoWait(deadDomains);
        });
    if (recovery.status == GpuValidatedLossStatus::Rejected) {
#ifdef OLR_UNIT_TEST
        if (allowTokenlessTestGate && m_gpuBeforeTokenlessRecoveryEnteredForTest) {
            m_gpuBeforeTokenlessRecoveryEnteredForTest->release();
            if (m_gpuContinueTokenlessRecoveryForTest)
                m_gpuContinueTokenlessRecoveryForTest->acquire();
        }
#else
        (void) allowTokenlessTestGate;
#endif
        recovery = lossMonitor.withCoordinatedTokenlessRecovery([&]() { return qsizetype(0); });
    }
    registry.drainWithBoundedWait(kDeviceLossReadbackDrainMs);
#ifdef OLR_UNIT_TEST
    m_gpuLastAbandonedRetainsForTest.store(recovery.abandoned, std::memory_order_release);
#endif
}

bool PlaybackWorker::completeCoordinatedGpuRebuild(bool consumeRebuildBudget) {
    if (m_gpuRecoveryParticipantId == 0 || m_gpuPendingRecoveryGeneration == 0) return false;
    auto& monitor = GpuDeviceLossMonitor::instance();
    const GpuRecoveryTicket ticket = monitor.beginRebuild(m_gpuRecoveryParticipantId);
    if (!ticket.isValid() || ticket.lossGeneration() != m_gpuPendingRecoveryGeneration)
        return false;

    const bool permitted = !consumeRebuildBudget || consumeGpuDeviceLossRebuildBudget();
    const bool rebuilt = permitted && rebuildGpuSpine();
    m_gpuRebuildDeferredForSuspend.store(false, std::memory_order_release);
#ifdef OLR_UNIT_TEST
    if (m_gpuBeforeRecoveryCommitForTest) {
        m_gpuBeforeRecoveryCommitForTest->release();
        if (m_gpuContinueRecoveryCommitForTest) m_gpuContinueRecoveryCommitForTest->acquire();
    }
#endif
    if (!monitor.clearForRebuild(ticket)) {
        m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::RebuildPending),
                                 std::memory_order_release);
        return false;
    }
    m_gpuLastHandledLossGeneration = m_gpuPendingRecoveryGeneration;
    m_gpuPendingRecoveryGeneration = 0;
    if (!rebuilt) {
        m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                                 std::memory_order_release);
    } else if (monitor.isLost()) {
        m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::RebuildPending),
                                 std::memory_order_release);
    }
    if (!rebuilt) {
        monitor.unregisterRecoveryParticipant(m_gpuRecoveryParticipantId);
        m_gpuRecoveryParticipantId = 0;
    }
    return true;
}

void PlaybackWorker::sanitizeCacheForDeviceLossLocked(OutputFrameCache* cache, int* recoveredFrames,
                                                      int* removedGpuFrames) {
    if (!cache) return;
    const int recovered = cache->replaceVideoFrames(cachedCpuSnapshotForDeviceLoss);
    OutputFrameCache::EvictedVideoFrames evictedUnrecoveredGpuFrames;
    const int removed = cache->removeVideoFramesIf(
        [](const FrameHandle& frame) { return frame.isGpuBacked(); }, &evictedUnrecoveredGpuFrames);
    collectEvictedGpuFramesLocked(evictedUnrecoveredGpuFrames);
    if (recoveredFrames) *recoveredFrames += recovered;
    if (removedGpuFrames) *removedGpuFrames += removed;
}

void PlaybackWorker::sanitizeTrackBufferForDeviceLossLocked(TrackBuffer* buffer,
                                                            int* recoveredFrames,
                                                            int* removedGpuFrames) {
    if (!buffer) return;
    TrackBuffer::EvictedFrames evictedRecoveredFrames;
    const int recovered =
        buffer->replaceFrames(cachedCpuSnapshotForDeviceLoss, &evictedRecoveredFrames);
    collectEvictedGpuFramesLocked(evictedRecoveredFrames);

    TrackBuffer::EvictedFrames evictedUnrecoveredGpuFrames;
    const int removed = buffer->removeFramesIf(
        [](const FrameHandle& frame) { return frame.isGpuBacked(); }, &evictedUnrecoveredGpuFrames);
    collectEvictedGpuFramesLocked(evictedUnrecoveredGpuFrames);
    if (recoveredFrames) *recoveredFrames += recovered;
    if (removedGpuFrames) *removedGpuFrames += removed;
}

void PlaybackWorker::handleGpuDeviceLoss() {
    auto& lossMonitor = GpuDeviceLossMonitor::instance();
    const uint64_t pendingGeneration = lossMonitor.currentLossGeneration();
    if (pendingGeneration != 0 && pendingGeneration == m_gpuLastHandledLossGeneration) return;
    const GpuPipelineState state = gpuPipelineState();
    if (state == GpuPipelineState::CpuFallback) return;
    if (state == GpuPipelineState::RebuildPending) {
        if (pendingGeneration == 0 && m_gpuPendingRecoveryGeneration == 0 &&
            std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire)) {
            m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::Gpu),
                                     std::memory_order_release);
            {
                QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
                if (m_outputRuntime)
                    m_outputRuntime->setGpuRhiContext(
                        std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire));
            }
            rebuildOutputEndpoints();
            m_forceLiveOutputSnapshotsOnNextAttach.store(true, std::memory_order_release);
            m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
            return;
        }
        const bool newerLossNeedsCleanup = pendingGeneration != 0 &&
                                           pendingGeneration != m_gpuPendingRecoveryGeneration &&
                                           pendingGeneration != m_gpuLastHandledLossGeneration;
        if (!newerLossNeedsCleanup) {
            if (m_gpuRebuildDeferredForSuspend.load(std::memory_order_acquire) &&
                !gpuLifecycleSuspended()) {
                resumeDeferredGpuRebuild();
            } else if (!gpuLifecycleSuspended() && m_gpuPendingRecoveryGeneration != 0 &&
                       completeCoordinatedGpuRebuild(true)) {
                {
                    QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
                    if (m_outputRuntime)
                        m_outputRuntime->setGpuRhiContext(
                            gpuPipelineState() == GpuPipelineState::Gpu
                                ? std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire)
                                : std::shared_ptr<GpuRhiContext>{});
                }
                rebuildOutputEndpoints();
                if (gpuPipelineState() == GpuPipelineState::Gpu) {
                    m_forceLiveOutputSnapshotsOnNextAttach.store(true, std::memory_order_release);
                    m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
                }
            }
            return;
        }
    }

    detachOutputEndpointsForDeviceLoss();
    m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::RebuildPending),
                             std::memory_order_release);
    if (m_gpuRecoveryParticipantId == 0)
        m_gpuRecoveryParticipantId = lossMonitor.registerRecoveryParticipant();
    const uint64_t lossGeneration = lossMonitor.recordLoss();
    int recoveredFrames = 0;
    int removedGpuFrames = 0;
    OutputCommitResult recoveryCommit;
    {
        QMutexLocker locker(&m_mutex);
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (DecoderTrack* track : m_decoderBank)
            if (track)
                sanitizeTrackBufferForDeviceLossLocked(&track->buffer, &recoveredFrames,
                                                       &removedGpuFrames);
        for (DecoderTrack* track : m_prerollBank)
            if (track)
                sanitizeTrackBufferForDeviceLossLocked(&track->buffer, &recoveredFrames,
                                                       &removedGpuFrames);
        sanitizeCacheForDeviceLossLocked(m_outputCache.get(), &recoveredFrames, &removedGpuFrames);
        sanitizeCacheForDeviceLossLocked(m_stagingCache.get(), &recoveredFrames, &removedGpuFrames);
        sanitizeCacheForDeviceLossLocked(m_prerollStagingCache.get(), &recoveredFrames,
                                         &removedGpuFrames);
        m_gpuFrameRetireQueue = GpuFrameRetireQueue();
        const qint64 playhead = m_transport
                                    ? m_transport->currentPos()
                                    : m_lastVisiblePlayheadMs.load(std::memory_order_acquire);
        const std::optional<qint64> recoveredPlayhead =
            recoveredCachePlayheadLocked(playhead, lossGeneration);
        OutputCommit commit;
        commit.playheadMs =
            recoveredPlayhead.value_or(m_committedPlayheadMs.load(std::memory_order_acquire));
        commit.seekGeneration = m_committedGeneration.load(std::memory_order_acquire);
        commit.gpuGeneration = lossGeneration;
        commit.cacheAction = OutputCacheAction::Publish;
        commit.coverageMode = OutputCoverageMode::Displayable;
        commit.requireCurrentSeek = false;
        commit.guardPlayheadCache = true;
        commit.dispatch = PostCommitDispatch::Output;
        recoveryCommit = commitOutputStateLocked(commit);
    }
    drainGpuDeviceLossEvents();

    // Release the surfaces held for readback. Previously the readback retainer was
    // left untouched on device loss, so every held surface plus its now-dead fence
    // leaked forever (the fence's completedValue() can never reach its target once
    // the device is gone). A REAL loss carries a DeadDeviceToken minted at the
    // driver-authoritative detection site, so we free without waiting (waiting on a
    // dead fence would hang). An INJECTED loss (test) has no token, so we drain with
    // a bounded per-fence wait — safe because the live Null/WARP device's fences do
    // advance. LOCK RULE: the readback retainer has its own leaf mutex; this takes
    // no m_bufferMutex, matching the fence resets below.
    cleanupGpuRetirementsForDeviceLoss(true);

    // LOCK RULE: this method is entered from the worker decode thread with no
    // m_bufferMutex held. Do not wait old fences here; a removed device may never
    // advance its timeline.
    m_decodeFence.reset();
    std::atomic_store_explicit(&m_renderFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    std::atomic_store_explicit(&m_stagingFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    m_stagedFenceValue.store(0, std::memory_order_release);
    std::atomic_store_explicit(&m_gpuRhi, std::shared_ptr<GpuRhiContext>{},
                               std::memory_order_release);
#ifdef _WIN32
    m_winGpuImportEdge.reset();
    m_winGpuImportTried = false;
#endif

    m_gpuPendingRecoveryGeneration = lossGeneration;
    if (!lossMonitor.acknowledgeRecoveryCleanup(m_gpuRecoveryParticipantId, lossGeneration)) return;

    m_forceLiveOutputSnapshotsOnNextAttach.store(true, std::memory_order_release);
    if (gpuLifecycleSuspended()) {
        m_gpuRebuildDeferredForSuspend.store(true, std::memory_order_release);
        {
            QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
            if (m_outputRuntime) m_outputRuntime->setGpuRhiContext(nullptr);
        }
        rebuildOutputEndpoints();
        if (recoveryCommit.committed && recoveryCommit.dispatch == PostCommitDispatch::Output) {
            m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
            refreshOutputAfterSeekCommit();
        }
        return;
    }

    const bool recoveryCompleted = completeCoordinatedGpuRebuild(true);
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime)
            m_outputRuntime->setGpuRhiContext(
                gpuPipelineState() == GpuPipelineState::Gpu
                    ? std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire)
                    : std::shared_ptr<GpuRhiContext>{});
    }
    rebuildOutputEndpoints();
    if (recoveryCompleted && recoveryCommit.committed &&
        recoveryCommit.dispatch == PostCommitDispatch::Output) {
        m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
        refreshOutputAfterSeekCommit();
    }
}

void PlaybackWorker::sampleGpuMemoryPressure(qint64 nowMs) {
    if (!gpuPipelineEnabled()) return;
    if (gpuPipelineState() != GpuPipelineState::Gpu) return;
    if (gpuLifecycleSuspended()) return;
    if (m_memoryPressureLatched.load(std::memory_order_acquire)) return;

    const IosGpuLifecycleSink* sink = iosGpuLifecycleSink();
    const uint64_t warningCount = sink ? sink->memoryWarningCount() : 0;
    const bool memoryWarning = warningCount != m_lastIosMemoryWarningCount;
    if (memoryWarning) m_lastIosMemoryWarningCount = warningCount;

    const bool timeToPoll = nowMs - m_lastPressureSampleMs >= kPressurePollMs;
    const bool mintedBurst =
        GpuBudget::instance().mintedBytesSinceLastSample() >= kMintedBytesPressureSample;
    if (!memoryWarning && !timeToPoll && !mintedBurst) return;

    m_lastPressureSampleMs = nowMs;
    GpuBudget::instance().resetMintedBytesSinceLastSample();
    evaluateGpuMemoryPressure(iosAvailableMemoryBytes(), memoryWarning, nowMs);
}

qint64 PlaybackWorker::gpuMemoryPressureLevel1ThresholdBytes() {
    int width = 0;
    int height = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    int feedCount = 0;
    {
        QMutexLocker locker(&m_mutex);
        width = m_outputWidth;
        height = m_outputHeight;
        feedCount = m_outputFeedCount;
    }
    decodeSurfaceGeometryForGpuBudget(m_decoderBank, width, height, &surfaceWidth, &surfaceHeight);

    GpuBudgetConfig config;
    config.width = width;
    config.height = height;
    config.surfaceWidth = surfaceWidth;
    config.surfaceHeight = surfaceHeight;
    config.surfaceFormat = FramePixelFormat::Nv12;
    const qint64 frameBytes = qMax<qint64>(1, config.surfaceBytes());
    return qMax<qint64>(256 * kMiB, qint64(4) * qMax(1, feedCount) * frameBytes);
}

bool PlaybackWorker::deriveGpuBudgetFromAvailableMemory(uint64_t availableBytes) {
    if (gpuForcedPerTrackBudget() > 0) return false;
    if (availableBytes == 0 || availableBytes > uint64_t(kMaxSaneAvailableMemoryBytes))
        return false;

    const qint64 available = qint64(availableBytes);
    const qint64 gated = GpuBudget::instance().gatedLiveBytes();
    const qint64 derived = qMin<qint64>((available + gated) / 2, available);
    const qint64 floorBytes = qMin<qint64>(512 * kMiB, available);
    const qint64 clamped = qMin<qint64>(4 * kGiB, qMax(floorBytes, derived));
    GpuBudget::instance().setBudgetBytesForRuntime(clamped);
    return true;
}

void PlaybackWorker::evaluateGpuMemoryPressure(uint64_t availableBytes, bool memoryWarning,
                                               qint64 nowMs) {
    if (!gpuPipelineEnabled()) return;
    if (m_memoryPressureLatched.load(std::memory_order_acquire)) return;

    const bool sampleValid =
        availableBytes > 0 && availableBytes <= uint64_t(kMaxSaneAvailableMemoryBytes);
    if (sampleValid) deriveGpuBudgetFromAvailableMemory(availableBytes);

    const qint64 level1Threshold = gpuMemoryPressureLevel1ThresholdBytes();
    const qint64 available =
        sampleValid ? qint64(availableBytes) : std::numeric_limits<qint64>::max();
    const bool belowLevel1 = sampleValid && available < level1Threshold;
    if (!memoryWarning && !belowLevel1) return;

    const bool secondWarning = memoryWarning && m_lastPressureWarningMs >= 0 &&
                               nowMs - m_lastPressureWarningMs <= kSecondWarningLatchMs;
    if (memoryWarning) m_lastPressureWarningMs = nowMs;

    handleGpuMemoryPressureLevel1(nowMs);

    // iOS can jetsam this process shortly after headroom crosses the level-1
    // floor. Treat a real low-headroom sample as a hard fallback signal; keep
    // memory-warning-only samples as the softer trim path above.
    const bool belowLevel2 = sampleValid && available < level1Threshold;
    if (belowLevel2 || secondWarning) handleGpuMemoryPressureLevel2(nowMs);
}

void PlaybackWorker::handleGpuMemoryPressureLevel1(qint64 nowMs) {
    if (m_lastPressureLevel1Ms >= 0 && nowMs - m_lastPressureLevel1Ms < kPressurePollMs) return;
    m_lastPressureLevel1Ms = nowMs;
    ++m_counters.gpuMemoryPressureLevel1;

    const int64_t dur = qMax<int64_t>(1, frameDurMs());
    const int minTrailFrames = 8;
    const int64_t minTrailMs = minTrailFrames * dur;
    ResidencyWindowParams params = m_residencyWindowParams;
    params.trailMs = int(qMax<int64_t>(minTrailMs, qMax<int64_t>(params.trailMs / 2, minTrailMs)));
    params.chunkMs = qMax(1, qMin(params.chunkMs, params.leadMs));

    int width = 0;
    int height = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    int feedCount = 0;
    {
        QMutexLocker locker(&m_mutex);
        width = m_outputWidth;
        height = m_outputHeight;
        feedCount = m_outputFeedCount;
    }
    decodeSurfaceGeometryForGpuBudget(m_decoderBank, width, height, &surfaceWidth, &surfaceHeight);
    GpuBudgetConfig config;
    config.width = width;
    config.height = height;
    config.surfaceWidth = surfaceWidth;
    config.surfaceHeight = surfaceHeight;
    config.surfaceFormat = FramePixelFormat::Nv12;
    const qint64 perFeedSurfaces =
        qMax<qint64>(12, GpuBudget::instance().budgetBytes() /
                             qMax<qint64>(1, config.surfaceBytes()) / qMax(1, feedCount));
    params.globalFrameBudget = qMax(12, int(qMin<qint64>(qMax(1, feedCount) * perFeedSurfaces,
                                                         std::numeric_limits<int>::max())));
    params.perTrackCapOverride =
        qMax(12, int(qMin<qint64>(perFeedSurfaces, std::numeric_limits<int>::max())));
    m_residencyWindowParams = params;

    const qint64 playhead = m_transport ? m_transport->currentPos()
                                        : m_lastVisiblePlayheadMs.load(std::memory_order_acquire);
    const qint64 keepFrom = playhead - (windowTrailMs() + windowSlackMs());
    const qint64 keepTo = playhead + (windowLeadMs() + windowSlackMs());
    const qint64 audioKeepFrom = playhead - windowAudioTrailMs();
    const qint64 keepAudioFromSample = qMax<qint64>(0, audioKeepFrom * qint64(48000) / 1000);
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (DecoderTrack* track : m_decoderBank) {
            if (!track) continue;
            TrackBuffer::EvictedFrames evictedTrackFrames;
            track->buffer.trim(keepFrom, keepTo, &evictedTrackFrames);
            collectEvictedGpuFramesLocked(evictedTrackFrames);
        }
        if (m_outputCache) {
            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
            m_outputCache->trimWindow(keepFrom, keepTo, keepAudioFromSample, &evictedCacheFrames);
            collectEvictedGpuFramesLocked(evictedCacheFrames);
            publishOutputCacheLocked();
        }
        if (m_stagingCache) {
            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
            m_stagingCache->trimWindow(keepFrom, keepTo, keepAudioFromSample, &evictedCacheFrames);
            collectEvictedGpuFramesLocked(evictedCacheFrames);
        }
        if (m_prerollStagingCache) {
            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
            m_prerollStagingCache->trimWindow(keepFrom, keepTo, keepAudioFromSample,
                                              &evictedCacheFrames);
            collectEvictedGpuFramesLocked(evictedCacheFrames);
        }
    }
    flushNativeDecoderPools();
    drainEvictedGpuFrames();
}

void PlaybackWorker::flushNativeDecoderPools() {
    auto flushBank = [](QVector<DecoderTrack*>& bank) {
        for (DecoderTrack* track : bank) {
            if (track && track->nativeDecoder) track->nativeDecoder->flushExcessPixelBufferPool();
        }
    };
    flushBank(m_decoderBank);
    flushBank(m_prerollBank);
}

void PlaybackWorker::flushNativeDecoderPoolsThrottled(qint64 nowMs) {
    if (!gpuPathActive()) return;
    constexpr qint64 kPoolFlushIntervalMs = 250;
    if (m_lastNativeDecoderPoolFlushMs >= 0 &&
        nowMs - m_lastNativeDecoderPoolFlushMs < kPoolFlushIntervalMs) {
        return;
    }
    m_lastNativeDecoderPoolFlushMs = nowMs;
    flushNativeDecoderPools();
}

void PlaybackWorker::handleGpuMemoryPressureLevel2(qint64 nowMs) {
    Q_UNUSED(nowMs);
    bool expected = false;
    if (!m_memoryPressureLatched.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        return;
    }
    ++m_counters.gpuMemoryPressureLevel2;

    detachOutputEndpointsForDeviceLoss();
    m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                             std::memory_order_release);
    const uint64_t pressureGeneration = GpuGenerationCounter::instance().bump();

    for (DecoderTrack* track : m_decoderBank)
        if (track && track->nativeDecoder) track->nativeDecoder->reset();
    for (DecoderTrack* track : m_prerollBank)
        if (track && track->nativeDecoder) track->nativeDecoder->reset();

    int recoveredFrames = 0;
    int removedGpuFrames = 0;
    OutputCommitResult recoveryCommit;
    {
        QMutexLocker locker(&m_mutex);
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (DecoderTrack* track : m_decoderBank)
            if (track)
                sanitizeTrackBufferForDeviceLossLocked(&track->buffer, &recoveredFrames,
                                                       &removedGpuFrames);
        for (DecoderTrack* track : m_prerollBank)
            if (track)
                sanitizeTrackBufferForDeviceLossLocked(&track->buffer, &recoveredFrames,
                                                       &removedGpuFrames);
        sanitizeCacheForDeviceLossLocked(m_outputCache.get(), &recoveredFrames, &removedGpuFrames);
        sanitizeCacheForDeviceLossLocked(m_stagingCache.get(), &recoveredFrames, &removedGpuFrames);
        sanitizeCacheForDeviceLossLocked(m_prerollStagingCache.get(), &recoveredFrames,
                                         &removedGpuFrames);
        m_gpuFrameRetireQueue = GpuFrameRetireQueue();

        const qint64 playhead = m_transport
                                    ? m_transport->currentPos()
                                    : m_lastVisiblePlayheadMs.load(std::memory_order_acquire);
        const std::optional<qint64> recoveredPlayhead =
            recoveredCachePlayheadLocked(playhead, pressureGeneration);
        OutputCommit commit;
        commit.playheadMs =
            recoveredPlayhead.value_or(m_committedPlayheadMs.load(std::memory_order_acquire));
        commit.seekGeneration = m_committedGeneration.load(std::memory_order_acquire);
        commit.gpuGeneration = pressureGeneration;
        commit.cacheAction = OutputCacheAction::Publish;
        commit.coverageMode = OutputCoverageMode::Displayable;
        commit.requireCurrentSeek = false;
        commit.guardPlayheadCache = true;
        commit.dispatch = PostCommitDispatch::Output;
        recoveryCommit = commitOutputStateLocked(commit);
    }

    m_decodeFence.reset();
    std::atomic_store_explicit(&m_renderFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    std::atomic_store_explicit(&m_stagingFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    m_stagedFenceValue.store(0, std::memory_order_release);
    std::atomic_store_explicit(&m_gpuRhi, std::shared_ptr<GpuRhiContext>{},
                               std::memory_order_release);
#ifdef _WIN32
    m_winGpuImportEdge.reset();
    m_winGpuImportTried = false;
#endif
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) m_outputRuntime->setGpuRhiContext(nullptr);
    }
    rebuildOutputEndpoints();
    if (recoveryCommit.committed && recoveryCommit.dispatch == PostCommitDispatch::Output) {
        m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
        refreshOutputAfterSeekCommit();
    }
    if (m_gpuRecoveryParticipantId != 0) {
        GpuDeviceLossMonitor::instance().unregisterRecoveryParticipant(m_gpuRecoveryParticipantId);
        m_gpuRecoveryParticipantId = 0;
    }
    m_gpuPendingRecoveryGeneration = 0;
    m_gpuRebuildDeferredForSuspend.store(false, std::memory_order_release);
}

void PlaybackWorker::resumeDeferredGpuRebuild() {
    if (!m_gpuRebuildDeferredForSuspend.load(std::memory_order_acquire)) return;
    if (gpuLifecycleSuspended()) return;
    if (gpuPipelineState() != GpuPipelineState::RebuildPending) {
        m_gpuRebuildDeferredForSuspend.store(false, std::memory_order_release);
        return;
    }

    if (!completeCoordinatedGpuRebuild(false)) return;
    const bool rebuilt = gpuPipelineState() == GpuPipelineState::Gpu;
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime)
            m_outputRuntime->setGpuRhiContext(
                rebuilt ? std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire)
                        : std::shared_ptr<GpuRhiContext>{});
    }
    rebuildOutputEndpoints();
    if (rebuilt) {
        m_forceLiveOutputSnapshotsOnNextAttach.store(true, std::memory_order_release);
        m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
    }
}

void PlaybackWorker::detachOutputEndpointsForDeviceLoss() {
    QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
    if (m_outputRuntime) m_outputRuntime->setEndpoints({});
}

std::optional<qint64> PlaybackWorker::recoveredCachePlayheadLocked(qint64 playheadMs,
                                                                   uint64_t gpuGeneration) const {
    return outputCacheDisplayablePlayheadLocked(playheadMs, gpuGeneration);
}

bool PlaybackWorker::rebuildGpuSpine() {
    if (!gpuPipelineEnabled()) return false;
    if (gpuLifecycleSuspended()) return false;

    // Revoke every old backend's loss-mint authority before a replacement can be
    // created. A failed rebuild keeps the loss latch set; a later retry begins a
    // fresh authority epoch again.
    GpuDeviceLossMonitor::instance().beginRebuild();
    auto rhi = GpuRhiContext::create();
    if (!rhi || !rhi->isValid() || rhi->deviceLost()) return false;

    auto decodeFence = DecodeDoneFence::create();
    if (!decodeFence) return false;

#ifdef __APPLE__
    std::shared_ptr<GpuFence> renderFence = rhi->createFence();
    std::shared_ptr<GpuFence> stagingFence = rhi->createFence();
    if (!renderFence || !stagingFence) return false;
#elif defined(_WIN32)
    std::shared_ptr<GpuFence> renderFence;
    std::shared_ptr<GpuFence> stagingFence;
#else
    std::shared_ptr<GpuFence> renderFence = GpuFence::create();
    std::shared_ptr<GpuFence> stagingFence = GpuFence::create();
    if (!renderFence || !stagingFence) return false;
#endif

    std::atomic_store_explicit(&m_gpuRhi, std::move(rhi), std::memory_order_release);
    m_decodeFence = std::move(decodeFence);
    std::atomic_store_explicit(&m_renderFence, std::move(renderFence), std::memory_order_release);
    std::atomic_store_explicit(&m_stagingFence, std::move(stagingFence), std::memory_order_release);
    m_stagedFenceValue.store(0, std::memory_order_release);
    m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::Gpu), std::memory_order_release);
    return true;
}

void PlaybackWorker::configureGpuBudget() {
    if (!gpuPipelineEnabled()) return;

    FrameProvider* multiviewProvider = nullptr;
    FrameProvider* pgmProvider = nullptr;
    int feedCount = 0;
    int width = 0;
    int height = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    {
        QMutexLocker locker(&m_mutex);
        multiviewProvider = m_multiviewPreviewProvider;
        pgmProvider = m_pgmPreviewProvider;
        feedCount = m_outputFeedCount;
        width = m_outputWidth;
        height = m_outputHeight;
    }
    if (feedCount <= 0) return;

    decodeSurfaceGeometryForGpuBudget(m_decoderBank, width, height, &surfaceWidth, &surfaceHeight);

    int activeBusCount = qMax(1, feedCount);
    if (pgmProvider) ++activeBusCount;
    if (multiviewProvider) ++activeBusCount;

    GpuBudgetConfig cfg;
    cfg.feedCount = qMax(1, feedCount);
    const int64_t dur = qMax<int64_t>(1, frameDurMs());
    const int forcedBudget = gpuForcedPerTrackBudget();
    if (forcedBudget > 0) {
        cfg.aggregateDecodeWindow = forcedBudget * qMax(1, feedCount);
        cfg.stagingWindowPerFeed = 0;
        cfg.activeBusCount = activeBusCount;
        cfg.readbackRingDepth = 3;
    } else {
        cfg.aggregateDecodeWindow = qMax(1, m_residencyWindowParams.globalFrameBudget);
        cfg.stagingWindowPerFeed = int((int64_t(kStagingSpanMs) + dur - 1) / dur);
        cfg.activeBusCount = activeBusCount;
        cfg.readbackRingDepth = 3;
    }
    cfg.width = width;
    cfg.height = height;
    cfg.surfaceWidth = surfaceWidth;
    cfg.surfaceHeight = surfaceHeight;
    cfg.outputWidth = width;
    cfg.outputHeight = height;
    cfg.readbackWidth = width;
    cfg.readbackHeight = height;
    cfg.surfaceFormat = FramePixelFormat::Nv12;
    GpuBudget::instance().configure(cfg);
}

GpuPrefetchPlan PlaybackWorker::planGpuSeekPrefetchForReposition(int64_t target, int dir) {
    if (!gpuPipelineEnabled()) return {};

    int width = 0;
    int height = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    {
        QMutexLocker locker(&m_mutex);
        width = m_outputWidth;
        height = m_outputHeight;
    }
    decodeSurfaceGeometryForGpuBudget(m_decoderBank, width, height, &surfaceWidth, &surfaceHeight);

    GpuBudgetConfig surfaceConfig;
    surfaceConfig.width = width;
    surfaceConfig.height = height;
    surfaceConfig.surfaceWidth = surfaceWidth;
    surfaceConfig.surfaceHeight = surfaceHeight;
    surfaceConfig.surfaceFormat = FramePixelFormat::Nv12;

    GpuBudget& budget = GpuBudget::instance();
    const GpuPrefetchPlan plan = GpuSeekPrefetch::planPrefetch(
        target, dir, qMax<int64_t>(1, frameDurMs()), windowLeadMs(), surfaceConfig.surfaceBytes(),
        budget.budgetBytes(), budget.gatedLiveBytes());
    m_counters.gpuSeekPrefetchConsults++;
    m_counters.gpuSeekPrefetchPlannedSurfaces += plan.surfaceCount;
    return plan;
}

int64_t PlaybackWorker::manualSeekCommitFillTo(int64_t target, int64_t frameDurationMs,
                                               const GpuPrefetchPlan& prefetchPlan) {
    Q_UNUSED(prefetchPlan);
    return target + qMax<int64_t>(1, frameDurationMs);
}

#ifdef OLR_UNIT_TEST
int64_t PlaybackWorker::manualSeekCommitFillToForTest(int64_t target, int64_t frameDurationMs,
                                                      const GpuPrefetchPlan& prefetchPlan) {
    return manualSeekCommitFillTo(target, frameDurationMs, prefetchPlan);
}
#endif

GpuPrefetchPlan PlaybackWorker::beginGpuSeekPrefetchForReposition(int64_t target, int dir) {
    m_gpuSeekPrefetchActive = false;
    m_gpuSeekPrefetchRemaining = 0;
    m_gpuSeekPrefetchPlan = {};
    if (!gpuPipelineEnabled()) return {};

    GpuPrefetchPlan plan = planGpuSeekPrefetchForReposition(target, dir);
    m_gpuSeekPrefetchActive = true;
    m_gpuSeekPrefetchRemaining = plan.surfaceCount;
    m_gpuSeekPrefetchPlan = plan;
    return plan;
}

void PlaybackWorker::endGpuSeekPrefetchForReposition() {
    m_gpuSeekPrefetchActive = false;
    m_gpuSeekPrefetchRemaining = 0;
    m_gpuSeekPrefetchPlan = {};
}

bool PlaybackWorker::allowNativeGpuDecodeForCurrentPacket(int64_t packetPtsMs) {
    if (!gpuPipelineEnabled()) return false;
    if (!m_gpuSeekPrefetchActive) return true;
    if (m_gpuSeekPrefetchRemaining <= 0) return false;
    if (m_gpuSeekPrefetchPlan.surfaceCount <= 0) return false;
    if (packetPtsMs < m_gpuSeekPrefetchPlan.startMs || packetPtsMs > m_gpuSeekPrefetchPlan.endMs)
        return false;

    --m_gpuSeekPrefetchRemaining;
    ++m_counters.gpuSeekPrefetchGpuAttempts;
    return true;
}
#endif

void PlaybackWorker::initializeOutputGraph(int feedCount, int width, int height) {
    shutdownOutputGraph();
    {
        QMutexLocker locker(&m_mutex);
        m_outputFeedCount = qMax(0, feedCount);
        m_outputWidth = qMax(2, width);
        m_outputHeight = qMax(2, height);
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    uint64_t graphGeneration = 0;
    gpuResetFrameReadToCpuCount();
    m_gpuDeviceLossEvents.store(0, std::memory_order_release);
    m_gpuLastObservedLossCount.store(GpuDeviceLossMonitor::instance().lossCount(),
                                     std::memory_order_release);
    m_injectGpuDeviceLossForTest.store(false, std::memory_order_release);
    m_forceLiveOutputSnapshotsOnNextAttach.store(false, std::memory_order_release);
    m_forceLiveOutputSnapshots.store(0, std::memory_order_release);
    m_gpuDeviceLossRebuildsRemaining.store(kDeviceLossRebuildBudget, std::memory_order_release);
    m_gpuRebuildDeferredForSuspend.store(false, std::memory_order_release);
    m_memoryPressureLatched.store(false, std::memory_order_release);
    m_lastIosMemoryWarningCount = 0;
    m_lastPressureSampleMs = 0;
    m_lastPressureWarningMs = -1;
    m_lastPressureLevel1Ms = -1;
    m_lastNativeDecoderPoolFlushMs = -1;
    m_gpuLastHandledLossGeneration = 0;
    m_gpuPendingRecoveryGeneration = 0;
    m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                             std::memory_order_release);
    std::atomic_store_explicit(&m_gpuRhi, std::shared_ptr<GpuRhiContext>{},
                               std::memory_order_release);
    m_decodeFence.reset();
    std::atomic_store_explicit(&m_renderFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    std::atomic_store_explicit(&m_stagingFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    m_stagedFenceValue.store(0, std::memory_order_release);
    if (gpuPipelineEnabled()) {
        auto& lossMonitor = GpuDeviceLossMonitor::instance();
        const GpuRecoveryRegistration registration =
            lossMonitor.registerRecoveryParticipantSnapshot(false);
        m_gpuRecoveryParticipantId = registration.participantId();
        graphGeneration = GpuGenerationCounter::instance().current();
        if (registration.lossGeneration() != 0) {
            m_gpuPendingRecoveryGeneration = registration.lossGeneration();
            m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::RebuildPending),
                                     std::memory_order_release);
            if (gpuLifecycleSuspended()) {
                m_gpuRebuildDeferredForSuspend.store(true, std::memory_order_release);
            } else {
                (void) completeCoordinatedGpuRebuild(false);
            }
        } else if (m_gpuRecoveryParticipantId != 0 && !rebuildGpuSpine()) {
            lossMonitor.unregisterRecoveryParticipant(m_gpuRecoveryParticipantId);
            m_gpuRecoveryParticipantId = 0;
        }
    }
    configureGpuBudget();
#endif
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
        m_outputCache =
            std::make_unique<OutputFrameCache>(m_outputFeedCount, m_outputWidth, m_outputHeight);
        // Publish the initial (empty) cache so the output thread loads a valid
        // snapshot from its very first tick instead of the inline fallback.
        publishOutputCacheLocked();
    }
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        m_outputRuntime = std::make_unique<OutputRuntime>(
            m_transport->frameRate(), m_outputFeedCount, m_outputWidth, m_outputHeight,
            std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire));
        m_outputRuntime->setSnapshotProvider([this]() { return makeOutputSnapshot(); });
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    if (graphGeneration != 0) {
        QMutexLocker locker(&m_mutex);
        QMutexLocker bufferLocker(&m_bufferMutex);
        OutputCommit commit;
        commit.playheadMs = m_committedPlayheadMs.load(std::memory_order_acquire);
        commit.seekGeneration = m_committedGeneration.load(std::memory_order_acquire);
        commit.gpuGeneration = graphGeneration;
        commit.cacheAction = OutputCacheAction::Publish;
        commit.requireCurrentSeek = false;
        commit.guardPlayheadCache = true;
        commit.dispatch = PostCommitDispatch::None;
        const OutputCommitResult graphCommit = commitOutputStateLocked(commit);
        Q_ASSERT(graphCommit.committed);
    }
#endif
    m_outputTargetsDirty.store(true, std::memory_order_relaxed);
    rebuildOutputEndpoints();
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) m_outputRuntime->startRuntime();
    }
}

void PlaybackWorker::shutdownOutputGraph() {
    std::unique_ptr<OutputRuntime> runtime;
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        while (m_outputRuntimeImmediateDispatches > 0)
            m_outputRuntimeImmediateDispatchesIdle.wait(&m_outputRuntimeMutex);
        runtime = std::move(m_outputRuntime);
    }
    if (runtime) {
        runtime->stopRuntime();
        runtime.reset();
    }
    m_outputSinks.clear();
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
#ifdef OLR_GPU_PIPELINE_BUILD
        if (m_outputCache) collectEvictedGpuFramesLocked(m_outputCache->videoFramesSnapshot());
        if (m_stagingCache) collectEvictedGpuFramesLocked(m_stagingCache->videoFramesSnapshot());
        if (m_prerollStagingCache)
            collectEvictedGpuFramesLocked(m_prerollStagingCache->videoFramesSnapshot());
#endif
        // Drop the published snapshot so a post-teardown tick (if any) falls back
        // to an empty cache rather than holding stale frames from the old graph.
#ifdef OLR_GPU_PIPELINE_BUILD
        auto previous = m_publishedCache.publish(nullptr);
        if (previous) collectEvictedGpuFramesLocked(previous->videoFramesSnapshot());
#else
        m_publishedCache.publish(nullptr);
#endif
        m_outputCache.reset();
        m_stagingCache.reset();
        m_prerollStagingCache.reset();
    }
    {
        QMutexLocker locker(&m_mutex);
        m_outputFeedCount = 0;
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    const auto gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
    bool deviceLost = GpuDeviceLossMonitor::instance().isLost();
    const bool rhiLost = gpuRhi && gpuRhi->pollDeviceLoss();
    const bool rhiPreviouslyLost = gpuRhi && gpuRhi->deviceLost();
#ifdef _WIN32
    // Poll every owned domain before taking the validated-proof snapshot. Do not
    // short-circuit after one backend reports loss: an adapter reset may retire
    // both QRhi and import-edge domains, and each exact identity must be published.
    const bool importEdgeLost = m_winGpuImportEdge && m_winGpuImportEdge->deviceLost();
    deviceLost = deviceLost || rhiLost || rhiPreviouslyLost || importEdgeLost;
#else
    deviceLost = deviceLost || rhiLost || rhiPreviouslyLost;
#endif
    if (deviceLost) {
        // Publish every still-observable old-domain proof and clean the process-wide
        // retirement registry before this graph acknowledges teardown.
        cleanupGpuRetirementsForDeviceLoss(false, false);
        QMutexLocker bufferLocker(&m_bufferMutex);
        m_gpuFrameRetireQueue = GpuFrameRetireQueue();
    } else {
        forceDrainEvictedGpuFrames();
    }
    std::atomic_store_explicit(&m_renderFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    std::atomic_store_explicit(&m_stagingFence, std::shared_ptr<GpuFence>{},
                               std::memory_order_release);
    m_stagedFenceValue.store(0, std::memory_order_release);
    m_decodeFence.reset();
    std::atomic_store_explicit(&m_gpuRhi, std::shared_ptr<GpuRhiContext>{},
                               std::memory_order_release);
    m_forceLiveOutputSnapshotsOnNextAttach.store(false, std::memory_order_release);
    m_forceLiveOutputSnapshots.store(0, std::memory_order_release);
    m_gpuPipelineState.store(static_cast<int>(GpuPipelineState::CpuFallback),
                             std::memory_order_release);
    m_memoryPressureLatched.store(false, std::memory_order_release);
    if (m_gpuRecoveryParticipantId != 0) {
        GpuDeviceLossMonitor::instance().unregisterRecoveryParticipant(m_gpuRecoveryParticipantId);
        m_gpuRecoveryParticipantId = 0;
    }
    m_gpuPendingRecoveryGeneration = 0;
#endif
}

void PlaybackWorker::rebuildOutputEndpoints() {
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return;
    }

    QList<OutputTargetAssignment> external;
    FrameProvider* multiviewProvider = nullptr;
    FrameProvider* pgmProvider = nullptr;
    bool feedPreviewProvidersEnabled = false;
    {
        QMutexLocker locker(&m_mutex);
        external = m_externalOutputAssignments;
        multiviewProvider = m_multiviewPreviewProvider;
        pgmProvider = m_pgmPreviewProvider;
        feedPreviewProvidersEnabled = m_feedPreviewProvidersEnabled.load(std::memory_order_acquire);
    }

    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return;
        m_outputRuntime->setEndpoints({});
    }
    m_outputSinks.clear();
    QList<OutputEndpoint> endpoints;
    std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks;
#ifdef OLR_GPU_PIPELINE_BUILD
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) sharedReadbacks = m_outputRuntime->sharedGpuReadbacks();
    }
#endif

    auto wrapForGpu = [&](std::unique_ptr<IOutputSink> sink, OutputBusId bus, OutputTargetKind kind,
                          FramePixelFormat format) -> std::unique_ptr<IOutputSink> {
#ifndef OLR_GPU_PIPELINE_BUILD
        (void) bus;
        (void) kind;
        (void) format;
#endif
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPathActive()) {
            Q_UNUSED(bus);
            const int depth = kind == OutputTargetKind::QtPreview ? 1 : 3;
            // Qt previews are operator feedback surfaces: scrub/jog must show the
            // latest playhead with minimum latency. Dispatched outputs keep the
            // deeper async ring for throughput and cadence stability.
            return std::make_unique<AsyncGpuReadbackSink>(
                std::move(sink), depth, format, gpuCapabilityFor(kind),
                std::atomic_load_explicit(&m_renderFence, std::memory_order_acquire),
                sharedReadbacks);
        }
#endif
        return sink;
    };

    const int feedPreviewCount = feedPreviewProvidersEnabled ? m_outputFeedCount : 0;
    const QList<OutputTargetAssignment> previews = BroadcastOutputSettings::qtPreviewAssignments(
        feedPreviewCount, multiviewProvider != nullptr, pgmProvider != nullptr);
    for (const OutputTargetAssignment& preview : previews) {
        FrameProvider* provider = nullptr;
        switch (preview.sourceBus.kind) {
        case OutputBusKind::Feed:
            if (preview.sourceBus.index >= 0 && preview.sourceBus.index < m_providers.size())
                provider = m_providers[preview.sourceBus.index];
            break;
        case OutputBusKind::Multiview:
            provider = multiviewProvider;
            break;
        case OutputBusKind::Pgm:
            provider = pgmProvider;
            break;
        }
        if (!provider) continue;

        auto sink = wrapForGpu(std::make_unique<QtPreviewOutputSink>(provider), preview.sourceBus,
                               OutputTargetKind::QtPreview, FramePixelFormat::Yuv420p);
        endpoints.append({preview, sink.get()});
        m_outputSinks.push_back(std::move(sink));
    }

    std::shared_ptr<GpuFence> ioRenderFence;
#ifdef OLR_GPU_PIPELINE_BUILD
    ioRenderFence = std::atomic_load_explicit(&m_renderFence, std::memory_order_acquire);
#endif

    for (const OutputTargetAssignment& assignment : external) {
        if (!assignment.enabled) continue;

        std::unique_ptr<IOutputSink> sink;
        switch (assignment.kind) {
        case OutputTargetKind::Ndi:
            sink =
                wrapForGpu(std::make_unique<QueuedOutputSink>(std::make_unique<NdiOutputSink>()),
                           assignment.sourceBus, OutputTargetKind::Ndi, FramePixelFormat::Yuv420p);
            break;
        case OutputTargetKind::QtPreview:
            break; // handled by the preview loop above; not expected in external list
        case OutputTargetKind::DeckLinkSdiHdmi:
        case OutputTargetKind::DeckLinkIpSt2110:
        case OutputTargetKind::Omt:
        case OutputTargetKind::Aja:
            sink = makeIoTargetSink(assignment, m_transport->frameRate(), ioRenderFence,
                                    sharedReadbacks);
            break;
        }
        if (!sink) continue;
        endpoints.append({assignment, sink.get()});
        m_outputSinks.push_back(std::move(sink));
    }

#ifdef OLR_GPU_PIPELINE_BUILD
    if (m_forceLiveOutputSnapshotsOnNextAttach.exchange(false, std::memory_order_acq_rel))
        m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
#endif
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) m_outputRuntime->setEndpoints(endpoints);
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    configureGpuBudget();
#endif
    m_outputTargetsDirty.store(false, std::memory_order_relaxed);
}

void PlaybackWorker::publishOutputCacheLocked() {
    if (!m_outputCache) return;
    auto next = std::make_shared<const OutputFrameCache>(*m_outputCache);
#ifdef OLR_GPU_PIPELINE_BUILD
    m_publishedCache.publish(std::move(next));
#else
    m_publishedCache.publish(std::move(next));
#endif
}

#ifdef OLR_GPU_PIPELINE_BUILD
void PlaybackWorker::collectEvictedGpuFramesLocked(
    const TrackBuffer::EvictedFrames& evictedFrames) {
    for (const TrackBuffer::Frame& evicted : evictedFrames)
        collectEvictedGpuFrameLocked(evicted.frame);
}

void PlaybackWorker::collectEvictedGpuFramesLocked(
    const OutputFrameCache::EvictedVideoFrames& evictedFrames) {
    for (const FrameHandle& frame : evictedFrames)
        collectEvictedGpuFrameLocked(frame);
}

void PlaybackWorker::collectEvictedGpuFrameLocked(const FrameHandle& frame) {
    m_gpuFrameRetireQueue.collect(frame);
}

void PlaybackWorker::drainEvictedGpuFrames() {
    if (GpuDeviceLossMonitor::instance().isLost()) return;
    GpuRetireRegistry{}.drainCompleted();

    GpuFrameRetireQueue local;
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
        if (m_gpuFrameRetireQueue.isEmpty()) return;
        m_gpuFrameRetireQueue.swap(local);
    }

    constexpr int kRetireFenceWaitTimeoutMs = 1;
    constexpr int kMaxRetireFenceWaitsPerDrain = 1;
    int stalls = 0;
    local.drain(kRetireFenceWaitTimeoutMs, &stalls, kMaxRetireFenceWaitsPerDrain);
    for (int i = 0; i < stalls; ++i)
        recordFenceWaitStall();

    if (gpuDeviceLossPending()) return;

    if (!local.isEmpty()) {
        QMutexLocker bufferLocker(&m_bufferMutex);
        m_gpuFrameRetireQueue.append(std::move(local));
    }
    GpuRetireRegistry{}.drainCompleted();
}

void PlaybackWorker::forceDrainEvictedGpuFrames() {
    GpuRetireRegistry{}.drainCompleted();

    constexpr int kForceRetireFenceWaitTimeoutMs = 10;
    constexpr int kMaxForceRetireFenceWaitsPerPass = 1;
    constexpr int kMaxForceRetireFenceWaitPasses = 100;
    int passes = 0;

    while (passes++ < kMaxForceRetireFenceWaitPasses) {
        GpuFrameRetireQueue local;
        {
            QMutexLocker bufferLocker(&m_bufferMutex);
            if (m_gpuFrameRetireQueue.isEmpty()) break;
            m_gpuFrameRetireQueue.swap(local);
        }

        int stalls = 0;
        local.drain(kForceRetireFenceWaitTimeoutMs, &stalls, kMaxForceRetireFenceWaitsPerPass);
        for (int i = 0; i < stalls; ++i)
            recordFenceWaitStall();

        if (gpuDeviceLossPending()) break;

        if (!local.isEmpty()) {
            QMutexLocker bufferLocker(&m_bufferMutex);
            m_gpuFrameRetireQueue.append(std::move(local));
            if (passes >= kMaxForceRetireFenceWaitPasses) {
                // Process teardown must not hang forever on a broken backend fence.
                m_gpuFrameRetireQueue = GpuFrameRetireQueue();
                break;
            }
        }
    }

    GpuRetireRegistry{}.drainCompleted();
}

void PlaybackWorker::recordFenceWaitStall() {
    QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
    if (m_outputRuntime) m_outputRuntime->incrementFenceWaitStalls();
}

bool PlaybackWorker::ensureWindowsGpuImportFencesReadyForDecode() {
#if defined(_WIN32)
    if (!m_winGpuImportEdge) return false;
    auto renderFence = std::atomic_load_explicit(&m_renderFence, std::memory_order_acquire);
    auto stagingFence = std::atomic_load_explicit(&m_stagingFence, std::memory_order_acquire);
    if (!renderFence) {
        renderFence = m_winGpuImportEdge->createFence();
        std::atomic_store_explicit(&m_renderFence, renderFence, std::memory_order_release);
    }
    if (!stagingFence) {
        stagingFence = m_winGpuImportEdge->createFence();
        std::atomic_store_explicit(&m_stagingFence, stagingFence, std::memory_order_release);
    }
    return renderFence && stagingFence;
#else
    return false;
#endif
}
#endif

OutputRuntimeSnapshot PlaybackWorker::makeOutputSnapshot() const {
    // Tier3 armed cut: read the dispatcher's next output frame index BEFORE
    // locking m_bufferMutex (dispatcherNextOutputFrameIndex locks the output
    // runtime's OWN mutex; taking it here avoids any m_bufferMutex -> runtime
    // m_mutex ordering). The cut promotion mutates worker-owned state from the
    // output thread by design (the swap/republish must be atomic w.r.t. this
    // snapshot), so we const_cast this const provider to drive it under
    // m_bufferMutex below.
    qint64 dispatcherNextIndex = 0;
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime)
            dispatcherNextIndex = m_outputRuntime->dispatcherNextOutputFrameIndex();
    }

    OutputRuntimeSnapshot snapshot;
    qint64 committedPlayhead = 0;
    uint64_t committedGen = 0;
    uint64_t seekGen = 0;
    bool forceLiveCacheSnapshot = false;
    PostCommitDispatch postCommitDispatch = PostCommitDispatch::None;
    {
        // Tier 2: read the immutable published snapshot instead of deep-copying
        // the live m_outputCache on every ~1ms tick. The slot's load() takes one
        // short lock and returns a shared_ptr to a const cache the worker never
        // mutates again, so the assignment below copies an implicitly-shared
        // (cheap COW) snapshot, not a re-decode of the decoder track buffers.
        PlaybackWorker* const mutableThis = const_cast<PlaybackWorker*>(this);
        QMutexLocker workerLocker(&mutableThis->m_mutex);
        QMutexLocker bufferLocker(&m_bufferMutex);
        // Fire the scheduled cut (if due) while holding the canonical
        // m_mutex -> m_bufferMutex order, BEFORE reading the published cache so
        // this tick paints the promoted window.
        postCommitDispatch = mutableThis->maybeFireScheduledCut(dispatcherNextIndex);
        workerLocker.unlock();
#ifdef OLR_GPU_PIPELINE_BUILD
        int forcedLiveSnapshots = m_forceLiveOutputSnapshots.load(std::memory_order_acquire);
        while (forcedLiveSnapshots > 0) {
            if (m_forceLiveOutputSnapshots.compare_exchange_weak(
                    forcedLiveSnapshots, forcedLiveSnapshots - 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                forceLiveCacheSnapshot = true;
                break;
            }
        }
#endif
        forceLiveCacheSnapshot =
            forceLiveCacheSnapshot || m_outputPlayheadCacheGuarded.load(std::memory_order_acquire);
        if (forceLiveCacheSnapshot && m_outputCache) {
            snapshot.cache = *m_outputCache;
        } else if (auto published = m_publishedCache.load()) {
            snapshot.cache = *published;
        } else {
            snapshot.cache = OutputFrameCache(m_outputFeedCount, m_outputWidth, m_outputHeight);
        }
        committedPlayhead = m_committedPlayheadMs.load(std::memory_order_acquire);
        committedGen = m_committedGeneration.load(std::memory_order_acquire);
        seekGen = m_seekGeneration.load(std::memory_order_acquire);
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPipelineEnabled()) {
            snapshot.state.gpuGeneration =
                (committedGen == seekGen)
                    ? GpuGenerationCounter::instance().current()
                    : m_committedGpuGeneration.load(std::memory_order_acquire);
        }
#endif
    }

    if (postCommitDispatch == PostCommitDispatch::Output) {
#ifdef OLR_GPU_PIPELINE_BUILD
        m_forceLiveOutputSnapshots.store(64, std::memory_order_release);
#endif
        const_cast<PlaybackWorker*>(this)->refreshOutputAfterSeekCommit();
    }

#ifdef OLR_GPU_PIPELINE_BUILD
    const_cast<PlaybackWorker*>(this)->drainEvictedGpuFrames();
#endif

    // Gate the visible playhead behind the committed cache generation: when no
    // seek is outstanding (committedGen == seekGen) this returns the LIVE
    // transport playhead so 1x playback advances every tick; while a reposition
    // for the latest seek is in flight it holds the last committed playhead so
    // the snapshot never reports a new playhead against a not-yet-ready cache.
    snapshot.state.selectedFeedIndex = m_selectedOutputFeed.load(std::memory_order_relaxed);
    if (snapshot.state.selectedFeedIndex < 0 && m_outputFeedCount > 0)
        snapshot.state.selectedFeedIndex = 0;

    const qint64 transportPlayhead = m_transport ? m_transport->currentPos() : 0;
    const bool transportPlaying = m_transport && m_transport->isPlaying();
    snapshot.state.playheadMs =
        CommitGate::visiblePlayheadMs(transportPlayhead, committedPlayhead, committedGen, seekGen);
    if (committedGen == seekGen) {
        const qint64 bookmarkedPlayhead = m_lastVisiblePlayheadMs.load(std::memory_order_acquire);
        const qint64 toleranceMs = qMax<qint64>(1, frameDurMs());
        const bool requireAllFeeds =
            m_requireAllOutputFeedsForPlayhead.load(std::memory_order_acquire);
        auto feedCoverageAt = [&](qint64 playheadMs, int feedIndex) -> std::optional<qint64> {
            Q_UNUSED(toleranceMs);
            return outputFeedCoverageInCache(snapshot.cache, feedIndex, playheadMs,
                                             snapshot.state.gpuGeneration,
                                             OutputCoverageMode::Displayable);
        };
        auto coverageForPlayhead = [&](qint64 playheadMs, qint64* coveredPlayheadOut) -> bool {
            bool cacheCovered = m_outputFeedCount > 0;
            qint64 coveredPlayhead = std::numeric_limits<qint64>::min();
            if (requireAllFeeds) {
                for (int feed = 0; feed < m_outputFeedCount; ++feed) {
                    const std::optional<qint64> ptsMs = feedCoverageAt(playheadMs, feed);
                    if (!ptsMs.has_value()) {
                        cacheCovered = false;
                        break;
                    }
                    coveredPlayhead = coveredPlayhead == std::numeric_limits<qint64>::min()
                                          ? *ptsMs
                                          : qMin(coveredPlayhead, *ptsMs);
                }
            } else {
                const std::optional<qint64> ptsMs =
                    feedCoverageAt(playheadMs, snapshot.state.selectedFeedIndex);
                cacheCovered = ptsMs.has_value();
                if (cacheCovered) coveredPlayhead = *ptsMs;
            }
            if (coveredPlayhead == std::numeric_limits<qint64>::min()) coveredPlayhead = playheadMs;
            if (coveredPlayheadOut) *coveredPlayheadOut = coveredPlayhead;
            return cacheCovered;
        };
        auto displayablePlayheadInSnapshot = [&](qint64 playheadMs) -> std::optional<qint64> {
            auto candidateForFeed = [&](int feedIndex) -> std::optional<qint64> {
                if (const std::optional<qint64> covered = outputFeedCoverageInCache(
                        snapshot.cache, feedIndex, playheadMs, snapshot.state.gpuGeneration,
                        OutputCoverageMode::Displayable)) {
                    return covered;
                }
                const std::optional<FrameHandle> future =
                    snapshot.cache.firstFreshVideoFrameAtOrAfter(feedIndex, playheadMs,
                                                                 snapshot.state.gpuGeneration);
                if (!future.has_value() || future->metadata().key.isPlaceholder) {
                    const std::optional<FrameHandle> prior =
                        snapshot.cache.videoFrameAtFreshForGeneration(feedIndex, playheadMs,
                                                                      snapshot.state.gpuGeneration);
                    if (!prior.has_value() || prior->metadata().key.isPlaceholder)
                        return std::nullopt;
                    const qint64 ageMs = playheadMs - prior->metadata().key.ptsMs;
                    if (ageMs >= 0 && ageMs <= maxPriorCoverageMs()) return playheadMs;
                    return std::nullopt;
                }
                return future->metadata().key.ptsMs;
            };

            qint64 candidate = std::numeric_limits<qint64>::min();
            if (requireAllFeeds) {
                for (int feed = 0; feed < m_outputFeedCount; ++feed) {
                    const std::optional<qint64> feedCandidate = candidateForFeed(feed);
                    if (!feedCandidate.has_value()) return std::nullopt;
                    candidate = qMax(candidate, *feedCandidate);
                }
            } else {
                const std::optional<qint64> feedCandidate =
                    candidateForFeed(snapshot.state.selectedFeedIndex);
                if (!feedCandidate.has_value()) return std::nullopt;
                candidate = *feedCandidate;
            }
            if (candidate == std::numeric_limits<qint64>::min()) return std::nullopt;

            auto hasFrameAtCandidate = [&](int feedIndex) {
                const std::optional<FrameHandle> frame =
                    snapshot.cache.videoFrameAtFreshForGeneration(feedIndex, candidate,
                                                                  snapshot.state.gpuGeneration);
                return frame.has_value() && !frame->metadata().key.isPlaceholder;
            };
            if (requireAllFeeds) {
                for (int feed = 0; feed < m_outputFeedCount; ++feed) {
                    if (!hasFrameAtCandidate(feed)) return std::nullopt;
                }
            } else if (!hasFrameAtCandidate(snapshot.state.selectedFeedIndex)) {
                return std::nullopt;
            }
            return candidate;
        };
        qint64 coveredPlayhead = snapshot.state.playheadMs;
        bool cacheCovered = coverageForPlayhead(snapshot.state.playheadMs, &coveredPlayhead);
        qint64 bookmarkCoveredPlayhead = bookmarkedPlayhead;
        const bool bookmarkCovered =
            coverageForPlayhead(bookmarkedPlayhead, &bookmarkCoveredPlayhead);
        const qint64 unguardedPlayhead = snapshot.state.playheadMs;
        if (!cacheCovered) {
            if (const std::optional<qint64> displayable =
                    displayablePlayheadInSnapshot(unguardedPlayhead)) {
                snapshot.state.playheadMs = *displayable;
                coveredPlayhead = *displayable;
                cacheCovered = true;
            }
        }
        const bool bookmarkUsable = bookmarkCovered || (requireAllFeeds && bookmarkedPlayhead >= 0);
        if (coveredPlayhead == std::numeric_limits<qint64>::min())
            coveredPlayhead = snapshot.state.playheadMs;
        const qint64 guardedPlayhead = CommitGate::cacheGuardedVisiblePlayheadMs(
            snapshot.state.playheadMs, bookmarkedPlayhead, cacheCovered, bookmarkUsable,
            committedGen, seekGen);
        const bool guardActive = (guardedPlayhead != unguardedPlayhead);
        const bool wasGuarded =
            m_outputPlayheadCacheGuarded.exchange(guardActive, std::memory_order_acq_rel);
        snapshot.state.playheadMs = guardedPlayhead;
        snapshot.state.forcePlayEpochReset = guardActive || wasGuarded;
        const qint64 bookmark = CommitGate::bookmarkedVisiblePlayheadMs(
            bookmarkedPlayhead, snapshot.state.playheadMs, coveredPlayhead, cacheCovered,
            committedGen, seekGen);
        m_lastVisiblePlayheadMs.store(bookmark, std::memory_order_release);
    } else {
        const bool wasGuarded =
            m_outputPlayheadCacheGuarded.exchange(false, std::memory_order_acq_rel);
        snapshot.state.forcePlayEpochReset = wasGuarded;
    }
    snapshot.state.playing = transportPlaying;
    snapshot.state.speed = m_transport ? m_transport->speed() : 1.0;
    return snapshot;
}

// ---------------------------------------------------------------------------
// enqueueAudioFrame — format-guarded enqueue of active-view audio (spec §6.7).
// Mirrors the old pushAudioFrame format guard (S16 / 48k / stereo) but routes
// the PCM into the worker-side queue instead of pushing to AudioPlayer.
// ---------------------------------------------------------------------------
void PlaybackWorker::enqueueAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame,
                                       bool dedupTail) {
    if (audioFrame->format != AV_SAMPLE_FMT_S16 || audioFrame->sample_rate != 48000 ||
        audioFrame->ch_layout.nb_channels != 2) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning() << "PlaybackWorker: unsupported audio frame format" << audioFrame->format
                       << audioFrame->sample_rate << audioFrame->ch_layout.nb_channels;
        }
        return;
    }

    int64_t pts = audioFrame->pts;
    if (pts == AV_NOPTS_VALUE) pts = audioFrame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) return;

    const AVRational tb = m_fmtCtx->streams[aTrack->streamIndex]->time_base;
    const int64_t ptsMs = av_rescale_q(pts, tb, {1, 1000});

    // Dedup-before-decode after EOF un-latch (§6.8): a re-read tail cluster's
    // audio is already queued/played — skip it to avoid duplicate audio.
    if (dedupTail && aTrack->lastEnqueuedPtsMs >= 0 && ptsMs <= aTrack->lastEnqueuedPtsMs) return;

    const int dataSize =
        audioFrame->nb_samples * audioFrame->ch_layout.nb_channels * int(sizeof(int16_t));
    m_audioQueue.enqueue(ptsMs, reinterpret_cast<const char*>(audioFrame->data[0]), dataSize);
    aTrack->lastEnqueuedPtsMs = ptsMs;
}

void PlaybackWorker::cacheOutputAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame,
                                           bool dedupTail) {
    if (!m_outputCache) return;
    if (audioFrame->format != AV_SAMPLE_FMT_S16 || audioFrame->sample_rate != 48000 ||
        audioFrame->ch_layout.nb_channels != 2) {
        return;
    }

    int64_t pts = audioFrame->pts;
    if (pts == AV_NOPTS_VALUE) pts = audioFrame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) return;

    const AVRational tb = m_fmtCtx->streams[aTrack->streamIndex]->time_base;
    const int64_t ptsMs = av_rescale_q(pts, tb, {1, 1000});
    if (dedupTail && aTrack->lastCachedPtsMs >= 0 && ptsMs <= aTrack->lastCachedPtsMs) return;

    const int dataSize =
        audioFrame->nb_samples * audioFrame->ch_layout.nb_channels * int(sizeof(int16_t));
    MediaAudioFrame frame;
    frame.feedIndex = aTrack->viewIndex;
    frame.startSample = qMax<qint64>(0, ptsMs * qint64(48000) / 1000);
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.format = MediaSampleFormat::S16Interleaved;
    frame.pcm = QByteArray(reinterpret_cast<const char*>(audioFrame->data[0]), dataSize);

    QMutexLocker bufferLocker(&m_bufferMutex);
    // Insert only; the cache is republished once per batch (run-loop trim,
    // reposition merge) — never per-frame (that leaked the half-built staging
    // cache during a reposition and was O(N^2) on the decode hot path).
    if (m_outputCache) m_outputCache->insertAudioFrame(frame);
    aTrack->lastCachedPtsMs = ptsMs;
}

void PlaybackWorker::indexPrimaryVideoPacketForSeek(const DecoderTrack* track, const AVPacket* pkt,
                                                    qint64 framePtsMs) {
    if (!track || !pkt || m_decoderBank.isEmpty()) return;
    if (track->streamIndex != m_decoderBank[0]->streamIndex) return;
    if (pkt->pos < 0 || framePtsMs < 0) return;
    m_frameIndex.append(framePtsMs, static_cast<qint64>(pkt->pos));
}

// ---------------------------------------------------------------------------
// decodePacketIntoBank — decode one read packet into the bank.
//  * video: optional count-based decimation (§6.3); insert with window cap;
//           framesDropped++ on cap drop. dedupTail skips frames whose PTS is
//           <= the owning track's current newest (post-EOF re-read, §6.8).
//  * audio: keep ALL decoders warm; enqueue only the active view when audioOn.
// Returns the ms-PTS of the last video frame inserted, or INT64_MIN if none.
// Caller must NOT hold m_bufferMutex (we lock it for the insert).
// ---------------------------------------------------------------------------
int64_t PlaybackWorker::decodePacketIntoBank(AVPacket* pkt, AVFrame* vf, AVFrame* af, int64_t P,
                                             int dir, int trackCount, bool decimate,
                                             int decimateStep, bool audioOn, bool dedupTail) {
    int64_t lastVideoPtsMs = INT64_MIN;
    int cap = capFrames(trackCount);
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuPipelineEnabled()) {
        const int forcedBudget = gpuForcedPerTrackBudget();
        if (forcedBudget > 0) cap = forcedBudget;
    }
#endif
    // Protect the active fill range in the travel direction (spec §6.6) so the
    // cap can never evict a frame the window still needs:
    //   forward: [P, P + lead]   reverse: [P - lead, P]
    const int64_t leadMs = windowLeadMs();
    const int64_t protectLo = (dir >= 0) ? P : (P - leadMs);
    const int64_t protectHi = (dir >= 0) ? (P + leadMs) : P;

    for (auto* track : m_decoderBank) {
        if (pkt->stream_index != track->streamIndex) continue;

        // H.264 tracks use NativeVideoDecoder (hardware); all others use FFmpeg.
        if (track->nativeDecoder) {
            // Convert avcC length-prefixed packet → Annex B for the decoder.
            QByteArray annexB;
            const uint8_t* p = pkt->data;
            const uint8_t* end = p + pkt->size;
            static const char kStartCode[4] = {'\x00', '\x00', '\x00', '\x01'};
            while (p + 4 <= end) {
                const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                        (uint32_t(p[2]) << 8) | uint32_t(p[3]);
                p += 4;
                if (nalLen == 0 || p + nalLen > end) break;
                annexB.append(kStartCode, 4);
                annexB.append(reinterpret_cast<const char*>(p), int(nalLen));
                p += nalLen;
            }
            if (!annexB.isEmpty()) {
                AVRational tb = m_fmtCtx->streams[track->streamIndex]->time_base;
                const int64_t pktPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;

                CompressedAccessUnit unit;
                unit.codec = NativeVideoCodec::H264;
                unit.parameterSets = track->h264ParamSets;
                unit.pts90k =
                    (pktPts != AV_NOPTS_VALUE) ? av_rescale_q(pktPts, tb, {1, 90000}) : -1;
                unit.dts90k =
                    (pkt->dts != AV_NOPTS_VALUE) ? av_rescale_q(pkt->dts, tb, {1, 90000}) : -1;
                unit.annexB = annexB;

                auto packetPtsMs = [&]() -> int64_t {
                    if (pktPts != AV_NOPTS_VALUE) return av_rescale_q(pktPts, tb, {1, 1000});
                    return (lastVideoPtsMs != INT64_MIN) ? lastVideoPtsMs + frameDurMs() : P;
                };

                auto& counters = m_counters;
                auto* outputCache = m_outputCache.get();
                auto* bufferMutex = &m_bufferMutex;
#ifdef OLR_GPU_PIPELINE_BUILD
                auto renderFenceForCommit =
                    std::atomic_load_explicit(&m_renderFence, std::memory_order_acquire);
                auto* retireQueueForCommit = &m_gpuFrameRetireQueue;
                auto* outputRuntimeMutexForCommit = &m_outputRuntimeMutex;
                auto* outputRuntimeForCommit = &m_outputRuntime;
                [[maybe_unused]] auto expectedDecodeSurfaceBytesForTrack = [&]() -> qint64 {
                    GpuBudgetConfig cfg;
                    cfg.width = m_outputWidth;
                    cfg.height = m_outputHeight;
                    cfg.surfaceWidth = track->codecWidth;
                    cfg.surfaceHeight = track->codecHeight;
                    cfg.surfaceFormat = FramePixelFormat::Nv12;
                    return cfg.surfaceBytes();
                };
                auto collectEvictedGpuFrameForCommit = [&](const FrameHandle& frame) {
                    if (!retireQueueForCommit) return;
                    retireQueueForCommit->collect(frame);
                };
                auto collectEvictedTrackFramesForCommit =
                    [&](const TrackBuffer::EvictedFrames& evictedFrames) {
                        for (const TrackBuffer::Frame& evicted : evictedFrames)
                            collectEvictedGpuFrameForCommit(evicted.frame);
                    };
                auto collectEvictedCacheFramesForCommit =
                    [&](const OutputFrameCache::EvictedVideoFrames& evictedFrames) {
                        for (const FrameHandle& evicted : evictedFrames)
                            collectEvictedGpuFrameForCommit(evicted);
                    };
                auto drainEvictedGpuFramesForCommit = [&]() {
                    GpuRetireRegistry{}.drainCompleted();
                    if (!retireQueueForCommit) return;

                    GpuFrameRetireQueue local;
                    {
                        QMutexLocker bufferLocker(bufferMutex);
                        if (retireQueueForCommit->isEmpty()) return;
                        retireQueueForCommit->swap(local);
                    }

                    constexpr int kRetireFenceWaitTimeoutMs = 1;
                    constexpr int kMaxRetireFenceWaitsPerDrain = 1;
                    int stalls = 0;
                    local.drain(kRetireFenceWaitTimeoutMs, &stalls, kMaxRetireFenceWaitsPerDrain);
                    for (int i = 0; i < stalls; ++i) {
                        QMutexLocker runtimeLocker(outputRuntimeMutexForCommit);
                        if (outputRuntimeForCommit && *outputRuntimeForCommit)
                            (*outputRuntimeForCommit)->incrementFenceWaitStalls();
                    }

                    if (!local.isEmpty()) {
                        QMutexLocker bufferLocker(bufferMutex);
                        retireQueueForCommit->append(std::move(local));
                    }
                    GpuRetireRegistry{}.drainCompleted();
                };
#endif
                auto commitMediaFrame = [&](FrameHandle mediaFrame, int64_t framePtsMs) -> bool {
                    mediaFrame.metadata().key.ptsMs = framePtsMs;
                    mediaFrame.metadata().decodedSequence = ++m_decodedVideoSequence;
                    if (!mediaFrame.isPresentable()) return false;
                    indexPrimaryVideoPacketForSeek(track, pkt, framePtsMs);
                    {
                        QMutexLocker bufferLocker(bufferMutex);
                        TrackBuffer::EvictedFrames evictedTrackFrames;
                        if (!track->buffer.insert(framePtsMs, mediaFrame, cap, protectLo, protectHi,
                                                  &evictedTrackFrames)) {
                            counters.framesDropped++;
                        }
#ifdef OLR_GPU_PIPELINE_BUILD
                        collectEvictedTrackFramesForCommit(evictedTrackFrames);
#endif
                        if (outputCache) {
                            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                            outputCache->insertVideoFrame(mediaFrame, &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                            collectEvictedCacheFramesForCommit(evictedCacheFrames);
#endif
                        }
                    }
#ifdef OLR_GPU_PIPELINE_BUILD
                    drainEvictedGpuFramesForCommit();
#endif
                    counters.decodedVideoFrames++;
                    return true;
                };

#if defined(OLR_GPU_PIPELINE_BUILD) && defined(__APPLE__)
                auto gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
                if (GpuDeviceLossMonitor::instance().isLost() || (gpuRhi && gpuRhi->deviceLost())) {
                    handleGpuDeviceLoss();
                    gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
                    renderFenceForCommit =
                        std::atomic_load_explicit(&m_renderFence, std::memory_order_acquire);
                }
                auto decodeFence = m_decodeFence;
                auto renderFence = renderFenceForCommit;
                const qint64 expectedDecodeSurfaceBytes = expectedDecodeSurfaceBytesForTrack();
                const bool decodeBudgetHasHeadroom =
                    expectedDecodeSurfaceBytes <= 0 ||
                    GpuBudget::instance().canAllocate(expectedDecodeSurfaceBytes);
                if (gpuPathActive() && gpuRhi && renderFence && decodeBudgetHasHeadroom &&
                    allowNativeGpuDecodeForCurrentPacket(packetPtsMs())) {
                    const int savedDecimateCounter = track->decimateCounter;
                    bool gpuCallback = false;
                    bool gpuInserted = false;
                    bool gpuFallback = false;
                    auto handleSurface = [&](void* imageBuffer, qint64 /*pts90k*/) {
                        gpuCallback = true;
                        bool keep = true;
                        if (decimate) {
                            keep = (track->decimateCounter % decimateStep) == 0;
                            track->decimateCounter++;
                        }
                        if (!keep) return true;

                        const int64_t framePtsMs = packetPtsMs();
                        if (dedupTail) {
                            int64_t nv;
                            {
                                QMutexLocker bufferLocker(&m_bufferMutex);
                                nv = track->buffer.newestPts();
                            }
                            if (nv >= 0 && framePtsMs <= nv) return true;
                        }

                        FrameMetadata meta;
                        meta.key.feedIndex = track->feedIndex;
                        meta.key.ptsMs = framePtsMs;
                        meta.key.format = FramePixelFormat::Nv12;
                        meta.key.width = track->codecWidth;
                        meta.key.height = track->codecHeight;
                        meta.color = colorMetadataForNativeTrack(track);
                        meta.gpuGeneration = GpuGenerationCounter::instance().current();

                        auto surface =
                            wrapAppleImageBuffer(imageBuffer, gpuRhi->surfaceCompatibility());
                        auto cpuFallback = [surface, gpuRhi]() -> CpuPlanes {
                            return submitGpuReadback(gpuRhi, surface, FramePixelFormat::Yuv420p)
                                .planes;
                        };
                        GpuMintResult mint = mintGpuOrDegrade(std::move(surface), gpuRhi, meta,
                                                              renderFence, cpuFallback);
                        FrameHandle mediaFrame = std::move(mint.handle);
                        if (!mediaFrame.isPresentable()) {
                            if (gpuRhi->deviceLost()) {
                                // LOCK RULE: no m_bufferMutex is held here; the short dedup lock
                                // above has gone out of scope before GPU import/readback.
                                handleGpuDeviceLoss();
                                renderFenceForCommit.reset();
                            }
                            gpuFallback = true;
                            return false;
                        }
                        Q_ASSERT(mediaFrame.isPresentable());
                        if (decodeFence) decodeFence->signalDecodeDone();
                        gpuInserted = commitMediaFrame(mediaFrame, framePtsMs);
                        if (!gpuInserted) {
                            gpuFallback = true;
                            return false;
                        }
                        lastVideoPtsMs = framePtsMs;
                        return true;
                    };

                    QString gpuError;
                    const bool decodedSurface =
                        track->nativeDecoder->decodeKeepSurface(unit, handleSurface, &gpuError);
                    if (gpuInserted || (decodedSurface && gpuCallback && !gpuFallback)) {
                        return lastVideoPtsMs;
                    }

                    track->decimateCounter = savedDecimateCounter;
                    lastVideoPtsMs = INT64_MIN;
                }
#endif
#if defined(OLR_GPU_PIPELINE_BUILD) && defined(_WIN32)
                auto gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
                if (GpuDeviceLossMonitor::instance().isLost() || (gpuRhi && gpuRhi->deviceLost()) ||
                    (m_winGpuImportEdge && m_winGpuImportEdge->deviceLost())) {
                    handleGpuDeviceLoss();
                    gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
                    renderFenceForCommit =
                        std::atomic_load_explicit(&m_renderFence, std::memory_order_acquire);
                }
                if (gpuPathActive() && gpuRhi) {
                    if (!m_winGpuImportTried) {
                        QString importError;
                        m_winGpuImportEdge = WinGpuImportEdge::create(&importError);
                        m_winGpuImportTried = true;
                    }
                    if (m_winGpuImportEdge && m_winGpuImportEdge->isAvailable()) {
                        const bool fencesReady = ensureWindowsGpuImportFencesReadyForDecode();
                        if (!fencesReady) {
                            m_winGpuImportEdge.reset();
                        } else if (allowNativeGpuDecodeForCurrentPacket(packetPtsMs())) {
                            auto decodeFence = m_decodeFence;
                            renderFenceForCommit = std::atomic_load_explicit(
                                &m_renderFence, std::memory_order_acquire);
                            auto renderFence = renderFenceForCommit;
                            const int savedDecimateCounter = track->decimateCounter;
                            bool gpuCallback = false;
                            bool gpuInserted = false;
                            bool gpuFallback = false;
                            auto handleSurface = [&](void* mfSample, qint64 /*pts90k*/) {
                                gpuCallback = true;
                                bool keep = true;
                                if (decimate) {
                                    keep = (track->decimateCounter % decimateStep) == 0;
                                    track->decimateCounter++;
                                }
                                if (!keep) return true;

                                const int64_t framePtsMs = packetPtsMs();
                                if (dedupTail) {
                                    int64_t nv;
                                    {
                                        QMutexLocker bufferLocker(&m_bufferMutex);
                                        nv = track->buffer.newestPts();
                                    }
                                    if (nv >= 0 && framePtsMs <= nv) return true;
                                }

                                FrameMetadata meta;
                                meta.key.feedIndex = track->feedIndex;
                                meta.key.ptsMs = framePtsMs;
                                meta.key.format = FramePixelFormat::Nv12;
                                meta.key.width = track->codecWidth;
                                meta.key.height = track->codecHeight;
                                meta.color = colorMetadataForNativeTrack(track);
                                meta.gpuGeneration = GpuGenerationCounter::instance().current();

                                auto surface = m_winGpuImportEdge->tryImportSurface(
                                    mfSample, track->codecWidth, track->codecHeight);
                                auto cpuFallback = [surface, meta, renderFence]() -> CpuPlanes {
                                    FrameHandle fallback =
                                        WinGpuImportEdge::makeGpuFrameHandleForTest(surface, meta,
                                                                                    renderFence);
                                    return fallback.readToCpu(FramePixelFormat::Yuv420p);
                                };
                                GpuMintResult mint = mintGpuOrDegrade(
                                    surface, meta,
                                    [renderFence](std::shared_ptr<GpuSurface> genericSurface,
                                                  FrameMetadata frameMeta,
                                                  GpuBudgetCharge charge) -> FrameHandle {
                                        auto d3dSurface =
                                            std::dynamic_pointer_cast<D3D11GpuSurface>(
                                                genericSurface);
                                        if (!d3dSurface) return FrameHandle{};
                                        return WinGpuImportEdge::makeGpuFrameHandleForTest(
                                            std::move(d3dSurface), std::move(frameMeta),
                                            renderFence, std::move(charge));
                                    },
                                    cpuFallback);
                                FrameHandle imported = std::move(mint.handle);
                                if (!imported.isPresentable()) {
                                    if ((m_winGpuImportEdge && m_winGpuImportEdge->deviceLost()) ||
                                        (gpuRhi && gpuRhi->deviceLost())) {
                                        // LOCK RULE: no m_bufferMutex is held here; the short dedup
                                        // lock above has gone out of scope before GPU
                                        // import/readback.
                                        handleGpuDeviceLoss();
                                        renderFenceForCommit.reset();
                                    }
                                    gpuFallback = true;
                                    return false;
                                }
                                Q_ASSERT(imported.isPresentable());

                                if (decodeFence) decodeFence->signalDecodeDone();
                                gpuInserted = commitMediaFrame(std::move(imported), framePtsMs);
                                if (!gpuInserted) {
                                    gpuFallback = true;
                                    return false;
                                }
                                lastVideoPtsMs = framePtsMs;
                                return true;
                            };

                            QString gpuError;
                            const bool decodedSurface = track->nativeDecoder->decodeKeepSurface(
                                unit, handleSurface, &gpuError);
                            if (gpuInserted || (decodedSurface && gpuCallback && !gpuFallback)) {
                                return lastVideoPtsMs;
                            }

                            track->decimateCounter = savedDecimateCounter;
                            lastVideoPtsMs = INT64_MIN;
                        }
                    }
                }
#endif

                // Count-based decimation applies per decoded frame.
                auto handleFrame = [&](AVFrame* nativeVf) {
                    bool keep = true;
                    if (decimate) {
                        keep = (track->decimateCounter % decimateStep) == 0;
                        track->decimateCounter++;
                    }
                    if (!keep) {
                        av_frame_free(&nativeVf);
                        return;
                    }

                    // Restore PTS in ms from the packet's time_base.
                    const int64_t framePtsMs = packetPtsMs();

                    if (dedupTail) {
                        int64_t nv;
                        {
                            QMutexLocker bufferLocker(&m_bufferMutex);
                            nv = track->buffer.newestPts();
                        }
                        if (nv >= 0 && framePtsMs <= nv) {
                            av_frame_free(&nativeVf);
                            return;
                        }
                    }

                    // FP: `track` is an m_decoderBank element (always new'd) and is
                    // dereferenced unconditionally above, so it is never null here.
                    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
                    FrameHandle mediaFrame = convertToMediaVideoFrame(nativeVf, track->feedIndex);
                    commitMediaFrame(mediaFrame, framePtsMs);
                    lastVideoPtsMs = framePtsMs;
                    av_frame_free(&nativeVf);
                };

                // All-intra invariant: each access unit decodes to exactly one
                // keyframe, so handleFrame is called once per unit and the
                // per-frame decimateCounter/lastVideoPtsMs advancement matches
                // the FFmpeg path's per-receive_frame loop.
                QString decodeError;
                const bool decodedNative =
                    track->nativeDecoder->decode(unit, handleFrame, &decodeError);
                if (!decodedNative && track->nativeDecodeFailureWarnings < 3) {
                    ++track->nativeDecodeFailureWarnings;
                    qWarning() << "PlaybackWorker: NativeVideoDecoder failed for stream"
                               << track->streamIndex << "ptsMs" << packetPtsMs() << decodeError;
                }
            }
            return lastVideoPtsMs;
        }

        // Count-based decimation: keep every decimateStep-th decoded frame.
        // The keep-counter is per-track and advances per decoded *frame*, so a
        // DTS-bumped on-disk PTS lattice is irrelevant (spec §6.3).
        if (avcodec_send_packet(track->codecCtx, pkt) == 0) {
            while (avcodec_receive_frame(track->codecCtx, vf) == 0) {
                bool keep = true;
                if (decimate) {
                    keep = (track->decimateCounter % decimateStep) == 0;
                    track->decimateCounter++;
                }
                if (!keep) {
                    av_frame_unref(vf);
                    continue;
                }

                int64_t framePts = vf->pts;
                if (framePts == AV_NOPTS_VALUE) framePts = vf->best_effort_timestamp;
                AVRational tb = m_fmtCtx->streams[track->streamIndex]->time_base;
                int64_t framePtsMs;
                if (framePts != AV_NOPTS_VALUE) {
                    framePtsMs = av_rescale_q(framePts, tb, {1, 1000});
                } else {
                    // No PTS: synthesize from the last known, or fall back to P.
                    framePtsMs = (lastVideoPtsMs != INT64_MIN) ? lastVideoPtsMs + frameDurMs() : P;
                }

                indexPrimaryVideoPacketForSeek(track, pkt, framePtsMs);

                // Dedup-before-decode after EOF un-latch: a re-read tail cluster
                // already in the buffer is skipped (read cost only, no dup).
                if (dedupTail) {
                    int64_t nv;
                    {
                        QMutexLocker bufferLocker(&m_bufferMutex);
                        nv = track->buffer.newestPts();
                    }
                    if (nv >= 0 && framePtsMs <= nv) {
                        av_frame_unref(vf);
                        continue;
                    }
                }

                FrameHandle mediaFrame = convertToMediaVideoFrame(vf, track->feedIndex);
                mediaFrame.metadata().key.ptsMs = framePtsMs;
                mediaFrame.metadata().decodedSequence = ++m_decodedVideoSequence;
                if (mediaFrame.isValid()) {
                    {
                        QMutexLocker bufferLocker(&m_bufferMutex);
                        TrackBuffer::EvictedFrames evictedTrackFrames;
                        if (!track->buffer.insert(framePtsMs, mediaFrame, cap, protectLo, protectHi,
                                                  &evictedTrackFrames))
                            m_counters.framesDropped++;
#ifdef OLR_GPU_PIPELINE_BUILD
                        collectEvictedGpuFramesLocked(evictedTrackFrames);
#endif
                        // Insert only; republish is batched (run-loop trim /
                        // reposition merge), never per-frame — see enqueue note.
                        if (m_outputCache) {
                            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                            m_outputCache->insertVideoFrame(mediaFrame, &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                            collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif
                        }
                    }
#ifdef OLR_GPU_PIPELINE_BUILD
                    drainEvictedGpuFrames();
#endif
                    m_counters.decodedVideoFrames++;
                }
                lastVideoPtsMs = framePtsMs;
                av_frame_unref(vf);
            }
        }
        return lastVideoPtsMs;
    }

    // Audio: keep every decoder warm (instant view switch); enqueue active view.
    for (auto* aTrack : m_audioDecoderBank) {
        if (pkt->stream_index != aTrack->streamIndex) continue;
        if (avcodec_send_packet(aTrack->codecCtx, pkt) == 0) {
            while (avcodec_receive_frame(aTrack->codecCtx, af) == 0) {
                cacheOutputAudioFrame(aTrack, af, dedupTail);
                int activeView = m_activeAudioView.load(std::memory_order_relaxed);
                if (audioOn && activeView == aTrack->viewIndex) {
                    enqueueAudioFrame(aTrack, af, dedupTail);
                }
                av_frame_unref(af);
            }
        }
        return lastVideoPtsMs;
    }

    return lastVideoPtsMs;
}

// ---------------------------------------------------------------------------
// repositionTo (spec §6.2) — reuse fast-path or full trail-covering reposition.
// ---------------------------------------------------------------------------
void PlaybackWorker::repositionTo(int64_t target, int dir, AVPacket* pkt, AVFrame* vf, AVFrame* af,
                                  bool cutFollow) {
    const bool traceLatency = latencyTraceEnabled();
    QElapsedTimer traceTimer;
    if (traceLatency) traceTimer.start();
    const int trackCount = qMax(1, int(m_decoderBank.size()));
    const uint64_t startedSeekGeneration = m_seekGeneration.load(std::memory_order_acquire);

    // --- Reuse fast-path: every track already has a real frame at target. ---
    if (reuseAt(target)) {
        // A NEWER explicit seek arrived during this reposition (the full path breaks
        // on the same condition mid-fill). Do NOT commit a generation against a
        // target the operator has superseded: that would advance m_committedGeneration
        // to the new seek's value with a stale m_committedPlayheadMs, disengaging the
        // CommitGate and briefly exposing a frame the operator seeked away from (also
        // reachable via a decoder-follow reuse racing a manual seek). Return with the
        // gate still held; the run loop services the newer seek next, which commits.
        OutputCommitResult outputCommit;
        {
            QMutexLocker locker(&m_mutex);
            if (CommitGate::canCommitReposition(startedSeekGeneration,
                                                m_seekGeneration.load(std::memory_order_acquire),
                                                m_seekTargetMs >= 0)) {
                const bool operatorPgmObligationAvailable =
                    operatorPgmObligationAvailableLocked(startedSeekGeneration);
                {
                    QMutexLocker bufferLocker(&m_bufferMutex);
                    uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
                    if (gpuPipelineEnabled())
                        gpuGeneration = GpuGenerationCounter::instance().current();
#endif
                    OutputCommit commit;
                    commit.playheadMs = target;
                    commit.seekGeneration = startedSeekGeneration;
                    commit.gpuGeneration = gpuGeneration;
                    commit.coverageMode = OutputCoverageMode::OperatorSeek;
                    commit.dispatch = operatorPgmObligationAvailable
                                          ? PostCommitDispatch::PgmCritical
                                          : PostCommitDispatch::Output;
                    outputCommit = commitOutputStateLocked(commit);
                }
                if (outputCommit.committed) {
                    resetDedup();
                    deliverDueFrames(target, dir);
                    // Backward reuse-seek is an audio reposition (§6.7): clear + re-prime,
                    // never a silent re-release (AudioPlayer's overlap-trim would swallow it).
                    if (dir < 0) {
                        m_audioQueue.clear();
                        if (m_audioPlayer) m_audioPlayer->clear();
                    }
                    m_counters.reuseSeek++;
                }
            }
        }
        if (!outputCommit.committed) return;
        // The commit stores and epoch invalidation above share m_bufferMutex with
        // makeOutputSnapshot, so the gate cannot become visible before the re-anchor.
        // The immediate dispatch stays outside m_mutex and uses a fresh snapshot.
        if (outputCommit.dispatch == PostCommitDispatch::PgmCritical) {
            const OutputDispatchReport pgmReport = dispatchPgmCommitObligation(
                outputCommit.committedPlayheadMs, startedSeekGeneration);
            completeOperatorSeekTransaction(startedSeekGeneration, outputCommit.committedPlayheadMs,
                                            pgmReport);
        }
        QElapsedTimer refreshTimer;
        if (traceLatency) refreshTimer.start();
        if (outputCommit.dispatch == PostCommitDispatch::PgmCritical)
            refreshPreviewAfterSeekCommit();
        else if (outputCommit.dispatch == PostCommitDispatch::Output)
            refreshOutputAfterSeekCommit();
        if (traceLatency) {
            qInfo().noquote()
                << QStringLiteral(
                       "OLR_LATENCY reposition targetMs=%1 dir=%2 mode=reuse committed=1 "
                       "refreshNs=%3 totalNs=%4")
                       .arg(target)
                       .arg(dir)
                       .arg(refreshTimer.nsecsElapsed())
                       .arg(traceTimer.nsecsElapsed());
        }
        return;
    }

    // --- Full reposition: clear everything, seek behind target, fill forward. ---
    const bool invalidateGpuGeneration = CommitGate::shouldInvalidateGpuGenerationForReposition(
        startedSeekGeneration, m_committedGeneration.load(std::memory_order_acquire));
    clearDecoderBuffers(invalidateGpuGeneration);
    m_reverseAnchorMs = INT64_MAX; // a seek invalidates the reverse-fetch run
    m_audioQueue.clear();
    for (auto* aTrack : m_audioDecoderBank) {
        aTrack->lastEnqueuedPtsMs = -1;
        aTrack->lastCachedPtsMs = -1;
    }
    if (m_audioPlayer) m_audioPlayer->clear();

    const int64_t anchor = qMax<int64_t>(0, target - (dir < 0 ? windowLeadMs() : windowTrailMs()));
    const qint64 clearNs = traceLatency ? traceTimer.nsecsElapsed() : 0;

    const int primaryVideoStreamIndex = m_decoderBank[0]->streamIndex;
    AVStream* vStream = m_fmtCtx->streams[primaryVideoStreamIndex];

    if (m_fmtCtx->pb) {
        // The worker opens recordings while they are still growing. Matroska can
        // latch EOF after reading the temporary tail; a seek-driven reposition
        // must clear that latch before retrying or it can spin on the old EOF
        // without ever seeing newly appended clusters.
        m_fmtCtx->pb->eof_reached = 0;
        m_fmtCtx->pb->error = 0;
    }

    const bool exactSought = false;
    if (m_fmtCtx->pb) {
        m_fmtCtx->pb->eof_reached = 0;
        m_fmtCtx->pb->error = 0;
    }
    avformat_flush(m_fmtCtx);
    const int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
    int seekRet = av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
    if (seekRet < 0) {
        const AVRational avTimeBase{1, AV_TIME_BASE};
        const int64_t fileSeekPts = av_rescale_q(anchor, {1, 1000}, avTimeBase);
        seekRet = avformat_seek_file(m_fmtCtx, -1, INT64_MIN, fileSeekPts, fileSeekPts,
                                     AVSEEK_FLAG_BACKWARD);
    }
    if (seekRet >= 0) avformat_flush(m_fmtCtx);
    const qint64 seekNs = traceLatency ? traceTimer.nsecsElapsed() - clearNs : 0;
    // Drain the decoders' OUTPUT queues. Intra-only means the next packet decodes
    // standalone (no reference-frame priming needed), but a seek does NOT discard
    // frames already DECODED and queued before it: on a BACKWARD seek the first
    // avcodec_receive_frame would otherwise return a stale frame from the old
    // (forward) position, whose PTS trips the `newestPtsMin() >= fillTo` break
    // below so the fill aborts with the bank still parked forward — the reposition
    // then has to be retried (2-3 reactive backward-jumps) before the stale frames
    // drain. Flushing here makes a single reposition resync the bank deterministically.
    //
    // H.264 primary tracks have codecCtx == nullptr (they decode via nativeDecoder),
    // so they are intentionally NOT flushed here: NativeVideoDecoder::decode() drains
    // every frame for the submitted access unit before it returns (VideoToolbox
    // WaitForAsynchronousFrames / MediaFoundation drainSync) and re-supplies parameter
    // sets per call, so it holds NO inter-call output FIFO — there is no stale
    // post-seek frame to drain. This invariant is what makes a BACKWARD H.264 armed
    // cut (decoder-follow → cutFollow repositionTo) resync in a single pass, the same
    // as the flushed MPEG-2 path; e2e_play_armedcut_h264_back locks it in (a future
    // buffering/reordering native decoder would trip its reposition/held gates).
    for (auto* track : m_decoderBank)
        if (track->codecCtx) avcodec_flush_buffers(track->codecCtx);
    for (auto* aTrack : m_audioDecoderBank)
        if (aTrack->codecCtx) avcodec_flush_buffers(aTrack->codecCtx);

    // Decode forward through target + frameDurMs, inserting all tracks. A forward
    // GPU seek-prefetch with headroom extends this to the predicted lead window.
    // Audio is re-primed by the normal forward release after the reposition, so we
    // do NOT enqueue here (audio queue stays empty until forward fill repopulates it).
    int64_t fillTo = target + frameDurMs();
    int packets = 0;
    const int packetBudget = (capFrames(trackCount) + 4) * trackCount * 2;
    bool operatorPgmCompletedEarly = false;
    // Tier 2 double-buffer: decode the target window into a fresh staging cache,
    // then merge it into the live cache and trim old frames only AFTER coverage,
    // so the live cache is never momentarily empty at target during a far seek.
    if (m_outputFeedCount > 0) {
        if (!m_stagingCache)
            m_stagingCache = std::make_unique<OutputFrameCache>(m_outputFeedCount, m_outputWidth,
                                                                m_outputHeight);
        else {
            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
            m_stagingCache->clear(&evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
            QMutexLocker bufferLocker(&m_bufferMutex);
            collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif
        }
    }
    // Swap so decodePacketIntoBank's m_outputCache->insertVideoFrame lands in
    // staging, not the live cache (which keeps its old frames over the seek).
    std::unique_ptr<OutputFrameCache> liveSaved;
    if (m_stagingCache) {
        QMutexLocker bufferLocker(&m_bufferMutex);
        liveSaved = std::move(m_outputCache);
        m_outputCache = std::move(m_stagingCache);
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    const GpuPrefetchPlan repositionPrefetchPlan = beginGpuSeekPrefetchForReposition(target, dir);
    if (dir >= 0) fillTo = manualSeekCommitFillTo(target, frameDurMs(), repositionPrefetchPlan);
#endif

    while (!shouldInterrupt()) {
        // A newer explicit seek supersedes this fill.
        {
            QMutexLocker locker(&m_mutex);
            if (m_seekTargetMs >= 0) break;
        }

        const int ret = readPrimaryFrame(pkt);
        if (ret < 0) break; // EOF/short file: deliver what we have

        // Reposition decodes forward from the anchor; protect the [target,
        // target+kLead] span (dir=+1) — the trail below target is also kept by
        // the anchor being kTrailMs/kLeadMs below it.
        decodePacketIntoBank(pkt, vf, af, target, /*dir*/ 1, trackCount,
                             /*decimate*/ false, /*step*/ 1,
                             /*audioOn*/ false, /*dedupTail*/ false);
        av_packet_unref(pkt);

        maybeCompleteOperatorSeekAfterDecodedPacket(target, startedSeekGeneration,
                                                    operatorPgmCompletedEarly);

        if (++packets > packetBudget) break; // safety bound
        if (newestPtsMin() >= fillTo) break; // covered the target
    }
#ifdef OLR_GPU_PIPELINE_BUILD
    endGpuSeekPrefetchForReposition();
#endif
    const qint64 fillEndNs = traceLatency ? traceTimer.nsecsElapsed() : 0;

    qint64 commitTarget = target;
    bool targetCovered = m_outputFeedCount <= 0 || outputCacheCoversPlayhead(target);
    const bool allowDisplayableFallback =
        allowDisplayableFallbackForReposition(startedSeekGeneration);
    if (!targetCovered && allowDisplayableFallback && m_outputFeedCount > 0) {
        uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
        if (gpuPipelineEnabled()) gpuGeneration = GpuGenerationCounter::instance().current();
#endif
        QMutexLocker bufferLocker(&m_bufferMutex);
        if (const std::optional<qint64> displayable =
                outputCacheDisplayablePlayheadLocked(target, gpuGeneration)) {
            commitTarget = *displayable;
            targetCovered = true;
        }
    }
    if (!targetCovered) {
        {
            QMutexLocker bufferLocker(&m_bufferMutex);
            if (liveSaved) {
                m_stagingCache = std::move(m_outputCache);
                m_outputCache = std::move(liveSaved);
                publishOutputCacheLocked();
            }
        }
        {
            QMutexLocker locker(&m_mutex);
            if (m_seekTargetMs < 0 &&
                m_seekGeneration.load(std::memory_order_acquire) == startedSeekGeneration) {
                m_seekTargetMs = target;
            }
        }
        fprintf(stderr,
                "PlaybackWorker: reposition target %lldms not covered after fill; retrying\n",
                (long long) target);
        if (traceLatency) {
            qInfo().noquote()
                << QStringLiteral(
                       "OLR_LATENCY reposition targetMs=%1 dir=%2 mode=full committed=0 "
                       "anchorMs=%3 exact=%4 packets=%5 fillToMs=%6 clearNs=%7 seekNs=%8 "
                       "fillNs=%9 totalNs=%10")
                       .arg(target)
                       .arg(dir)
                       .arg(anchor)
                       .arg(exactSought ? 1 : 0)
                       .arg(packets)
                       .arg(fillTo)
                       .arg(clearNs)
                       .arg(seekNs)
                       .arg(fillEndNs - clearNs - seekNs)
                       .arg(traceTimer.nsecsElapsed());
        }
        msleep(kIdleSleepMs);
        return;
    }

    OutputCommitResult outputCommit;
    {
        QMutexLocker locker(&m_mutex);
        const bool canCommit = CommitGate::canCommitReposition(
            startedSeekGeneration, m_seekGeneration.load(std::memory_order_acquire),
            m_seekTargetMs >= 0);
        if (canCommit) {
            {
                QMutexLocker bufferLocker(&m_bufferMutex);
                uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
                if (gpuPipelineEnabled())
                    gpuGeneration = GpuGenerationCounter::instance().current();
#endif
                const bool operatorPgmObligationAvailable =
                    operatorPgmObligationAvailableLocked(startedSeekGeneration);
                OutputCommit commit;
                commit.playheadMs = commitTarget;
                commit.seekGeneration = startedSeekGeneration;
                commit.gpuGeneration = gpuGeneration;
                commit.cacheAction =
                    liveSaved ? OutputCacheAction::MergeStagingAndPublish : OutputCacheAction::Keep;
                commit.coverageMode = commitTarget == target ? OutputCoverageMode::OperatorSeek
                                                             : OutputCoverageMode::Displayable;
                commit.dispatch = operatorPgmObligationAvailable
                                      ? PostCommitDispatch::PgmCritical
                                      : (operatorPgmCompletedEarly ? PostCommitDispatch::Preview
                                                                   : PostCommitDispatch::Output);
                if (liveSaved) {
                    const qint64 keepFrom =
                        (dir < 0) ? commitTarget - (windowLeadMs() + windowSlackMs())
                                  : commitTarget - (windowTrailMs() + windowSlackMs());
                    const qint64 keepTo = (dir < 0)
                                              ? commitTarget + (windowTrailMs() + windowSlackMs())
                                              : commitTarget + (windowLeadMs() + windowSlackMs());
                    const qint64 keepAudioFromSample =
                        qMax<qint64>(0, keepFrom * qint64(48000) / 1000);
                    bool sanitizeForDeviceLoss = false;
#ifdef OLR_GPU_PIPELINE_BUILD
                    // Only strip GPU-backed frames when the device is actually lost:
                    // otherwise the freshly decoded seek-prefetch window (pure GPU, not
                    // yet read back to CPU) would be deleted on every healthy seek.
                    sanitizeForDeviceLoss = gpuDeviceLossPending();
#endif
                    outputCommit = commitFullRepositionOutputStateLocked(
                        commit, liveSaved, keepFrom, keepTo, keepAudioFromSample,
                        sanitizeForDeviceLoss);
                } else {
                    outputCommit = commitOutputStateLocked(commit);
                }
            }

            if (outputCommit.committed) {
                resetDedup();
                deliverDueFrames(outputCommit.committedPlayheadMs, dir);
                // An armed-cut decoder-follow resync counts separately: the output cache was
                // already promoted/correct by the cut, so this is NOT a coarse-seek fallback
                // (which the reposition counter / armed-cut gate guard against).
                if (cutFollow)
                    m_counters.cutFollowReposition++;
                else
                    m_counters.reposition++;
            }
        }
    }
    if (!outputCommit.committed) {
        if (liveSaved) {
            QMutexLocker bufferLocker(&m_bufferMutex);
            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
            if (m_outputCache) m_outputCache->clear(&evictedCacheFrames);
            m_stagingCache = std::move(m_outputCache);
            m_outputCache = std::move(liveSaved);
#ifdef OLR_GPU_PIPELINE_BUILD
            collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif
        }
#ifdef OLR_GPU_PIPELINE_BUILD
        drainEvictedGpuFrames();
#endif
        if (traceLatency) {
            qInfo().noquote()
                << QStringLiteral(
                       "OLR_LATENCY reposition targetMs=%1 dir=%2 mode=full committed=0 "
                       "superseded=1 anchorMs=%3 exact=%4 packets=%5 fillToMs=%6 totalNs=%7")
                       .arg(target)
                       .arg(dir)
                       .arg(anchor)
                       .arg(exactSought ? 1 : 0)
                       .arg(packets)
                       .arg(fillTo)
                       .arg(traceTimer.nsecsElapsed());
        }
        return;
    }

    // Tier 2: the cache now covers `target`; the committed playhead/generation were
    // published atomically with the cache above. Re-anchor and publish the target frame
    // immediately after leaving m_mutex so paused seek/step/scrub does not wait for the
    // next scheduled output tick.
    const qint64 commitNs = traceLatency ? traceTimer.nsecsElapsed() - fillEndNs : 0;
    if (outputCommit.dispatch == PostCommitDispatch::PgmCritical) {
        const OutputDispatchReport pgmReport =
            dispatchPgmCommitObligation(outputCommit.committedPlayheadMs, startedSeekGeneration);
        completeOperatorSeekTransaction(startedSeekGeneration, outputCommit.committedPlayheadMs,
                                        pgmReport);
    }
    QElapsedTimer refreshTimer;
    if (traceLatency) refreshTimer.start();
    if (outputCommit.dispatch == PostCommitDispatch::PgmCritical ||
        outputCommit.dispatch == PostCommitDispatch::Preview)
        refreshPreviewAfterSeekCommit();
    else if (outputCommit.dispatch == PostCommitDispatch::Output)
        refreshOutputAfterSeekCommit();
    const qint64 refreshNs = traceLatency ? refreshTimer.nsecsElapsed() : 0;

#ifdef OLR_GPU_PIPELINE_BUILD
    drainEvictedGpuFrames();
#endif
    if (traceLatency) {
        qInfo().noquote()
            << QStringLiteral(
                   "OLR_LATENCY reposition targetMs=%1 dir=%2 mode=full committed=1 "
                   "anchorMs=%3 commitTargetMs=%4 exact=%5 packets=%6 fillToMs=%7 clearNs=%8 "
                   "seekNs=%9 fillNs=%10 commitNs=%11 refreshNs=%12 totalNs=%13")
                   .arg(target)
                   .arg(dir)
                   .arg(anchor)
                   .arg(commitTarget)
                   .arg(exactSought ? 1 : 0)
                   .arg(packets)
                   .arg(fillTo)
                   .arg(clearNs)
                   .arg(seekNs)
                   .arg(fillEndNs - clearNs - seekNs)
                   .arg(commitNs)
                   .arg(refreshNs)
                   .arg(traceTimer.nsecsElapsed());
    }
}

// ---------------------------------------------------------------------------
// Tier3 pre-roll / armed-cut implementation.
//
// CONCURRENCY MODEL
//   * m_prerollFmtCtx / m_prerollBank / m_prerollAudioBank / m_prerollStagingCache
//     / m_stagingCovers / m_stagingNewestRefPtsMs are WORKER-THREAD-ONLY: opened,
//     filled and read solely inside run()/fillStaging on the worker thread. They
//     are NOT touched by the output thread, so the fill needs NO lock.
//   * The ONLY cross-thread handoff is the cut itself (maybeFireScheduledCut),
//     which runs on the OUTPUT thread inside makeOutputSnapshot under
//     m_bufferMutex. It swaps m_prerollStagingCache <-> m_outputCache (both
//     unique_ptr, worker-owned) and republishes — a single pointer swap. Because
//     the swap happens under m_bufferMutex (the same lock decodePacketIntoBank /
//     the run-loop trim take for m_outputCache mutation) the two pointers are
//     never read/written concurrently. After the cut the worker's fillStaging no
//     longer runs (m_cutArmed cleared), so the swapped-out (old live) cache that
//     now sits in m_prerollStagingCache is only re-touched on the NEXT arm, which
//     clears it first. v1 does not handle a manual seek racing an in-flight cut
//     (out of scope) — armNextCut + the makeOutputSnapshot cut are the only
//     writers of the schedule atomics.
// ---------------------------------------------------------------------------

// Mirrors the primary open/init in run() (alloc_context, interrupt_callback,
// avformat_open_input, find_stream_info, per-video-stream DecoderTrack +
// per-audio AudioDecoderTrack) but writes the preroll members. No provider
// wiring (the pre-roll feeds staging, not the live providers). Returns false on
// failure; the caller leaves pre-roll disabled. Worker thread only.
bool PlaybackWorker::openPrerollContext() {
    if (m_prerollFmtCtx) avformat_close_input(&m_prerollFmtCtx);

    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx) return false;
    ctx->interrupt_callback.callback = &PlaybackWorker::ffmpegInterruptCallback;
    ctx->interrupt_callback.opaque = this;
    if (avformat_open_input(&ctx, m_currentFilePath.toUtf8().constData(), nullptr, nullptr) < 0) {
        avformat_close_input(&ctx);
        return false;
    }
    m_prerollFmtCtx = ctx;
    if (avformat_find_stream_info(m_prerollFmtCtx, nullptr) < 0) {
        avformat_close_input(&m_prerollFmtCtx);
        return false;
    }

    // Build the pre-roll video bank, mapped 1:1 by stream order to feedIndex,
    // exactly like the primary loop (but capped at the provider count so the
    // feedIndex matches the live cache feeds).
    int feedIndex = 0;
    for (unsigned int i = 0; i < m_prerollFmtCtx->nb_streams; i++) {
        AVCodecParameters* codecParams = m_prerollFmtCtx->streams[i]->codecpar;
        if (codecParams->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (feedIndex >= m_providers.size()) break;

        // H.264: hardware-only licensing constraint — NEVER software-decode.
        // Mirror the primary bank guard: when HW is available and extradata is
        // usable, build a NativeVideoDecoder exactly as the primary bank does
        // (playbackworker.cpp:1463-1522) so the pre-roll bank can stage H.264
        // frames. When HW is unavailable or avcC parse fails, skip (continue)
        // so the bank stays empty and armNextCut returns false (graceful
        // degradation, feature off for that file).
        //
        // Homogeneous-codec invariant: OLR recordings are single-codec by
        // construction. Both layouts are now supported:
        //   • all-H.264  → NativeVideoDecoder built, feedIndex advances 1:1
        //   • all-MPEG-2 → no H.264 branch taken, existing FFmpeg path below
        // A hypothetical externally-authored MIXED-codec file is out of scope.
        if (codecParams->codec_id == AV_CODEC_ID_H264) {
            if (queryNativeVideoDecodeCapabilities().h264 && codecParams->extradata_size >= 8) {
                // Parse avcC extradata → SPS/PPS NAL payloads (verbatim from
                // the primary bank, playbackworker.cpp:1466-1522).
                H26xParameterSets params;
                bool parseOk = true;
                const uint8_t* ed = codecParams->extradata;
                const int edSize = codecParams->extradata_size;
                int off = 5;
                const int numSps = off < edSize ? (ed[off] & 0x1f) : 0;
                off++;
                for (int s = 0; s < numSps && parseOk; ++s) {
                    if (off + 2 > edSize) {
                        parseOk = false;
                        break;
                    }
                    const int len = (ed[off] << 8) | ed[off + 1];
                    off += 2;
                    if (len <= 0 || off + len > edSize) {
                        parseOk = false;
                        break;
                    }
                    params.h264Sps.append(QByteArray(reinterpret_cast<const char*>(ed + off), len));
                    off += len;
                }
                if (parseOk && off < edSize) {
                    const int numPps = ed[off++];
                    for (int p = 0; p < numPps && parseOk; ++p) {
                        if (off + 2 > edSize) {
                            parseOk = false;
                            break;
                        }
                        const int len = (ed[off] << 8) | ed[off + 1];
                        off += 2;
                        if (len <= 0 || off + len > edSize) {
                            parseOk = false;
                            break;
                        }
                        params.h264Pps.append(
                            QByteArray(reinterpret_cast<const char*>(ed + off), len));
                        off += len;
                    }
                }
                if (!parseOk || params.h264Sps.isEmpty() || params.h264Pps.isEmpty()) {
                    qWarning() << "PlaybackWorker: pre-roll H.264 avcC parse failed"
                               << "for stream" << i << "— skipping (HW-only constraint)";
                    continue;
                }
                DecoderTrack* track = new DecoderTrack();
                track->streamIndex = static_cast<int>(i);
                track->nativeDecoder =
                    std::make_unique<NativeVideoDecoder>(codecParams->width, codecParams->height);
                track->h264ParamSets = params;
                track->codecWidth = codecParams->width;
                track->codecHeight = codecParams->height;
                track->codecCtx = nullptr;
                track->provider = nullptr; // no live provider wiring for pre-roll
                track->feedIndex = feedIndex;
                m_prerollBank.append(track);
                feedIndex++;
                qDebug() << "PlaybackWorker: pre-roll NativeVideoDecoder (H.264)"
                         << "stream" << i << "feedIndex" << (feedIndex - 1);
                continue;
            }
            // No HW or bad extradata: skip — graceful degradation.
            continue;
        }

        const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) continue;
        AVCodecContext* cctx = avcodec_alloc_context3(codec);
        if (!cctx) continue;
        avcodec_parameters_to_context(cctx, codecParams);
        cctx->thread_count = 0;
        if (avcodec_open2(cctx, codec, nullptr) < 0) {
            avcodec_free_context(&cctx);
            continue;
        }
        DecoderTrack* track = new DecoderTrack();
        track->streamIndex = static_cast<int>(i);
        track->codecCtx = cctx;
        track->provider = nullptr; // no live provider wiring for pre-roll
        track->feedIndex = feedIndex;
        m_prerollBank.append(track);
        feedIndex++;
    }

    // Build the pre-roll audio bank (paired with video by order), like primary.
    int audioViewIdx = 0;
    for (unsigned int i = 0; i < m_prerollFmtCtx->nb_streams; i++) {
        AVCodecParameters* codecParams = m_prerollFmtCtx->streams[i]->codecpar;
        if (codecParams->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            audioViewIdx++;
            continue;
        }
        AVCodecContext* cctx = avcodec_alloc_context3(codec);
        if (!cctx) {
            audioViewIdx++;
            continue;
        }
        avcodec_parameters_to_context(cctx, codecParams);
        cctx->thread_count = 0;
        if (avcodec_open2(cctx, codec, nullptr) < 0) {
            avcodec_free_context(&cctx);
            audioViewIdx++;
            continue;
        }
        AudioDecoderTrack* aTrack = new AudioDecoderTrack();
        aTrack->streamIndex = static_cast<int>(i);
        aTrack->codecCtx = cctx;
        aTrack->viewIndex = audioViewIdx;
        m_prerollAudioBank.append(aTrack);
        audioViewIdx++;
    }

    if (m_prerollBank.isEmpty()) {
        avformat_close_input(&m_prerollFmtCtx);
        return false;
    }
    // Staging cache sized identically to m_outputCache.
    m_prerollStagingCache =
        std::make_unique<OutputFrameCache>(m_outputFeedCount, m_outputWidth, m_outputHeight);
    qDebug() << "PlaybackWorker: pre-roll context opened (" << m_prerollBank.size()
             << "video tracks )";
    return true;
}

// UI-thread-safe: atomic stores only, never blocks. Arms a scheduled atomic cut
// to targetMs. Returns false (feature unavailable) if the pre-roll context
// failed to open (e.g. H.264 recordings: the pre-roll bank is hardware-only-
// guarded and stays empty). Returns true when the cut is armed or queued for
// re-arm. v1 is single-clip (ms-only; same currently-open file).
bool PlaybackWorker::armNextCut(int64_t targetMs, int64_t fireAtPlayheadMs) {
    if (!m_prerollFmtCtx) return false; // pre-roll disabled — feature unavailable
    // Safe re-arm queue: a re-arm (rapid double "Recall") while a cut is already
    // armed/in-flight must NOT reset the staging state from this (UI) thread. The
    // worker fills m_prerollStagingCache lock-free and only stops once
    // m_stagingCovers is set; clearing it here would make the worker resume
    // clearing/inserting that cache concurrently with the output thread's swap in
    // maybeFireScheduledCut — a data race on a lock-free cache. Instead queue the
    // LATEST target; the run loop applies it (armCutInternal) the moment the
    // in-flight cut clears m_cutArmed, so the re-arm and its staging fill run
    // sequentially on the worker thread. Latest queued target wins.
    if (m_cutArmed.load(std::memory_order_acquire)) {
        m_pendingRearmMs.store(targetMs < 0 ? 0 : targetMs);
        m_pendingRearmFireAtMs.store(fireAtPlayheadMs, std::memory_order_relaxed);
        // Capture the seek generation NOW (queue time): if a manual seek lands
        // before the worker applies this re-arm, m_seekGeneration moves past it and
        // the worker drops the re-arm (the seek is the newer explicit action).
        m_pendingRearmSeekGen.store(m_seekGeneration.load(std::memory_order_acquire),
                                    std::memory_order_relaxed);
        m_hasPendingRearm.store(true, std::memory_order_release);
        return true; // queued for re-arm; the cut will still navigate to targetMs
    }
    // Fresh arm: armNextCut and seekTo are both UI-thread, so reading m_seekGeneration
    // here is a coherent baseline (no seek can interleave between this read and the arm).
    armCutInternal(targetMs, m_seekGeneration.load(std::memory_order_acquire), fireAtPlayheadMs);
    return true;
}

// Arm the cut state. Caller guarantees no cut is in flight (m_cutArmed false), so
// the subsequent worker-thread fillStaging never races the output swap. Atomics
// only (no cache touch) — safe from the UI thread (fresh arm) or the worker
// thread (queued re-arm applied in the run loop). m_cutArmed is released LAST so
// a worker observing it true (acquire) sees the target/seek-pending stores.
void PlaybackWorker::armCutInternal(int64_t targetMs, uint64_t baselineSeekGen,
                                    int64_t fireAtPlayheadMs) {
    m_armedTargetMs.store(targetMs < 0 ? 0 : targetMs);
    m_armedFireAtMs.store(fireAtPlayheadMs);
    m_prerollSeekPending.store(true);
    m_stagingCovers.store(false);
    m_scheduledCutFrame.store(-1);
#ifdef OLR_GPU_PIPELINE_BUILD
    m_stagedFenceValue.store(0, std::memory_order_release);
#endif
    // Baseline is the seek generation when the recall was ISSUED (not now): a manual
    // seekTo after that point bumps m_seekGeneration past it, and maybeFireScheduled
    // Cut aborts the cut on the mismatch (manual seek wins). Capturing it at arm time
    // instead would re-baseline a queued re-arm against a seek that raced the apply.
    m_armSeekGen.store(baselineSeekGen, std::memory_order_release);
    m_cutArmed.store(true, std::memory_order_release);
}

// Worker-thread-only. Bounded incremental pre-roll into m_prerollStagingCache.
// NOT published until the cut swap, so no lock during the fill. On the first
// call after arm, av_seek_frame's the pre-roll context BACKWARD to target-kTrailMs
// (a raw avio_seek into this MKV is unreliable — Part A proved it) then decodes
// forward until staging covers [target, target+kStagingSpanMs]; schedules the cut
// the moment coverage is reached.
void PlaybackWorker::fillStaging() {
    if (!m_prerollFmtCtx || m_prerollBank.isEmpty() || !m_prerollStagingCache) return;
    const int64_t target = m_armedTargetMs.load();
    if (target < 0 || m_stagingCovers.load()) return;

    const int primaryStreamIndex = m_prerollBank[0]->streamIndex;
    AVStream* refStream = m_prerollFmtCtx->streams[primaryStreamIndex];

    if (m_prerollSeekPending.exchange(false)) {
        const int64_t anchor = qMax<int64_t>(0, target - windowTrailMs());
        const int64_t seekPts = av_rescale_q(anchor, {1, 1000}, refStream->time_base);
        av_seek_frame(m_prerollFmtCtx, refStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
        avformat_flush(m_prerollFmtCtx);
        OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
        m_prerollStagingCache->clear(&evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
        {
            QMutexLocker bufferLocker(&m_bufferMutex);
            collectEvictedGpuFramesLocked(evictedCacheFrames);
        }
#endif
        m_stagingNewestRefPtsMs = INT64_MIN;
        // Step 3: reset native sessions post-seek so the VT/MF decoder starts
        // clean — guarantees PTS fidelity (maxClockDivergenceMs gate).
        // All-intra mezzanine makes this safe and cheap.
        for (auto* track : m_prerollBank) {
            if (track->nativeDecoder) track->nativeDecoder->reset();
        }
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* vf = av_frame_alloc();
    AVFrame* af = av_frame_alloc(); // Tier3 audio staging (active view)
    if (!pkt || !vf || !af) {
        if (pkt) av_packet_free(&pkt);
        if (vf) av_frame_free(&vf);
        if (af) av_frame_free(&af);
        return;
    }
    const int activeView = m_activeAudioView.load(std::memory_order_relaxed);

    const int64_t coverTo = target + kStagingSpanMs;
    int packets = 0;
    while (packets++ < kPrerollPacketsPerTick) {
        int ret = av_read_frame(m_prerollFmtCtx, pkt);
        if (ret < 0) {
            // EOF / short clip: take whatever we staged as "covering" so the cut
            // can still fire (it will land on the largest pts<=target available).
            markStagingCovered();
            av_packet_unref(pkt);
            break;
        }

        // Decode video packets into the staging cache (mirror decodePacketIntoBank's
        // video insert path, but into m_prerollStagingCache; no FrameIndex append,
        // no per-track buffer, no live cache touch).
        for (auto* track : m_prerollBank) {
            if (pkt->stream_index != track->streamIndex) continue;
            if (track->nativeDecoder) {
                // H.264 native path: mirrors decodePacketIntoBank native branch
                // (~:557-634) but writes m_prerollStagingCache (not m_outputCache)
                // and omits primary-bank-only state (m_bufferMutex, track->buffer).
                // NativeVideoDecoder::decode() is SYNCHRONOUS — the stage lambda
                // fires inline on this (worker) thread before decode() returns, so
                // m_stagingNewestRefPtsMs is updated before the coverage check below.
                // This holds on VideoToolbox (WaitForAsynchronousFrames blocks until
                // the output callback runs) and is the only configuration validated
                // here. An ASYNC MediaFoundation MFT (Windows) could defer delivery
                // past this call; that deeper coverage/PTS correctness is a known
                // limitation shared with the primary decode bank and is tracked as a
                // follow-up (the staging fill would then need a blocking per-AU drain).
                // The stage lambda captures the per-packet locals BY VALUE so a
                // (hypothetical) deferred callback can never read a stale/dangling
                // reference — behavior-identical to the inline VT path.
                AVRational tb = m_prerollFmtCtx->streams[track->streamIndex]->time_base;
                const int64_t pktPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
                // Convert avcC length-prefixed packet → Annex B (verbatim from
                // decodePacketIntoBank, ~:558-571).
                QByteArray annexB;
                {
                    const uint8_t* p = pkt->data;
                    const uint8_t* end = p + pkt->size;
                    static const char kStartCode[4] = {'\x00', '\x00', '\x00', '\x01'};
                    while (p + 4 <= end) {
                        const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                                (uint32_t(p[2]) << 8) | uint32_t(p[3]);
                        p += 4;
                        if (nalLen == 0 || p + nalLen > end) break;
                        annexB.append(kStartCode, 4);
                        annexB.append(reinterpret_cast<const char*>(p), int(nalLen));
                        p += nalLen;
                    }
                }
                if (!annexB.isEmpty()) {
                    CompressedAccessUnit unit;
                    unit.codec = NativeVideoCodec::H264;
                    unit.parameterSets = track->h264ParamSets;
                    unit.pts90k =
                        (pktPts != AV_NOPTS_VALUE) ? av_rescale_q(pktPts, tb, {1, 90000}) : -1;
                    unit.dts90k =
                        (pkt->dts != AV_NOPTS_VALUE) ? av_rescale_q(pkt->dts, tb, {1, 90000}) : -1;
                    unit.annexB = annexB;

                    auto stage = [this, pktPts, tb, target, track,
                                  primaryStreamIndex](AVFrame* nativeVf) {
                        int64_t framePtsMs;
                        if (pktPts != AV_NOPTS_VALUE) {
                            framePtsMs = av_rescale_q(pktPts, tb, {1, 1000});
                        } else {
                            framePtsMs = target;
                        }
                        FrameHandle mediaFrame =
                            convertToMediaVideoFrame(nativeVf, track->feedIndex);
                        mediaFrame.metadata().key.ptsMs = framePtsMs;
                        mediaFrame.metadata().decodedSequence = ++m_decodedVideoSequence;
                        if (mediaFrame.isValid()) {
                            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                            m_prerollStagingCache->insertVideoFrame(mediaFrame,
                                                                    &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                            if (!evictedCacheFrames.isEmpty()) {
                                QMutexLocker bufferLocker(&m_bufferMutex);
                                collectEvictedGpuFramesLocked(evictedCacheFrames);
                            }
#endif
                            m_counters.stagingVideoFramesDecoded++;
                            if (track->streamIndex == primaryStreamIndex) {
                                m_stagingNewestRefPtsMs = qMax(m_stagingNewestRefPtsMs, framePtsMs);
                            }
                        }
                        av_frame_free(&nativeVf);
                    };
                    track->nativeDecoder->decode(unit, stage, nullptr);
                }
            } else {
                // FFmpeg software path (MPEG-2, etc.) — unchanged.
                if (avcodec_send_packet(track->codecCtx, pkt) == 0) {
                    while (avcodec_receive_frame(track->codecCtx, vf) == 0) {
                        int64_t framePts = vf->pts;
                        if (framePts == AV_NOPTS_VALUE) framePts = vf->best_effort_timestamp;
                        int64_t framePtsMs;
                        if (framePts != AV_NOPTS_VALUE) {
                            framePtsMs = av_rescale_q(
                                framePts, m_prerollFmtCtx->streams[track->streamIndex]->time_base,
                                {1, 1000});
                        } else {
                            framePtsMs = target;
                        }
                        FrameHandle mediaFrame = convertToMediaVideoFrame(vf, track->feedIndex);
                        mediaFrame.metadata().key.ptsMs = framePtsMs;
                        mediaFrame.metadata().decodedSequence = ++m_decodedVideoSequence;
                        if (mediaFrame.isValid()) {
                            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                            m_prerollStagingCache->insertVideoFrame(mediaFrame,
                                                                    &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                            if (!evictedCacheFrames.isEmpty()) {
                                QMutexLocker bufferLocker(&m_bufferMutex);
                                collectEvictedGpuFramesLocked(evictedCacheFrames);
                            }
#endif
                            m_counters.stagingVideoFramesDecoded++;
                            if (track->streamIndex == primaryStreamIndex)
                                m_stagingNewestRefPtsMs = qMax(m_stagingNewestRefPtsMs, framePtsMs);
                        }
                        av_frame_unref(vf);
                    }
                }
            }
            break;
        }

        // Stage the ACTIVE-VIEW audio for the armed window into the (worker-
        // private) staging cache so the output bus has the target's audio the
        // instant the cut promotes it — mirrors cacheOutputAudioFrame but writes
        // m_prerollStagingCache (no lock; not published until the cut swap) and
        // uses the pre-roll context's time base. Bounded to [.., target+span].
        for (auto* aTrack : m_prerollAudioBank) {
            if (pkt->stream_index != aTrack->streamIndex) continue;
            if (aTrack->viewIndex == activeView &&
                avcodec_send_packet(aTrack->codecCtx, pkt) == 0) {
                while (avcodec_receive_frame(aTrack->codecCtx, af) == 0) {
                    if (af->format == AV_SAMPLE_FMT_S16 && af->sample_rate == 48000 &&
                        af->ch_layout.nb_channels == 2) {
                        int64_t apts = af->pts;
                        if (apts == AV_NOPTS_VALUE) apts = af->best_effort_timestamp;
                        if (apts != AV_NOPTS_VALUE) {
                            const AVRational atb =
                                m_prerollFmtCtx->streams[aTrack->streamIndex]->time_base;
                            const int64_t aPtsMs = av_rescale_q(apts, atb, {1, 1000});
                            if (aPtsMs <= target + kPrerollAudioSpanMs) {
                                const int dataSize = af->nb_samples * 2 * int(sizeof(int16_t));
                                MediaAudioFrame frame;
                                frame.feedIndex = aTrack->viewIndex;
                                frame.startSample = qMax<qint64>(0, aPtsMs * qint64(48000) / 1000);
                                frame.sampleRate = 48000;
                                frame.channels = 2;
                                frame.format = MediaSampleFormat::S16Interleaved;
                                frame.pcm = QByteArray(reinterpret_cast<const char*>(af->data[0]),
                                                       dataSize);
                                m_prerollStagingCache->insertAudioFrame(frame);
                            }
                        }
                    }
                    av_frame_unref(af);
                }
            }
            break;
        }
        av_packet_unref(pkt);

        if (m_stagingNewestRefPtsMs >= coverTo) {
            markStagingCovered();
            break;
        }
    }

    av_frame_free(&vf);
    av_frame_free(&af);
    av_packet_free(&pkt);

#ifdef OLR_GPU_PIPELINE_BUILD
    drainEvictedGpuFrames();
#endif

    // Schedule the cut the moment staging first covers the target window — unless
    // the cut was disarmed meanwhile (e.g. maybeFireScheduledCut aborted it on a
    // manual-seek generation mismatch, clearing m_cutArmed under m_bufferMutex
    // while this fill was in flight). Re-checking m_cutArmed here closes the
    // lost-update window where an aborted cut would be re-scheduled.
    if (m_cutArmed.load(std::memory_order_acquire) && m_stagingCovers.load() &&
        m_scheduledCutFrame.load() < 0) {
        const qint64 lead = CutSchedule::framesForLeadMs(kCutLeadMs, fps());
        const qint64 nextIdx =
            m_outputRuntime ? m_outputRuntime->dispatcherNextOutputFrameIndex() : 0;
        // Earliest safe fire frame: the staging is covered now, so kCutLeadMs ahead.
        const qint64 asapFrame = CutSchedule::outputFrameForCut(nextIdx, static_cast<int>(lead));
        qint64 fireFrame = asapFrame;
        // Frame-perfect playout: when a fire-at-playhead was armed (a playlist
        // entry's out-point), fire at the output frame where the sampled playhead
        // reaches it — but never before the earliest safe frame (if the out-point is
        // already imminent or past, fall back to firing as-soon-as-staged).
        const int64_t fireAt = m_armedFireAtMs.load();
        if (fireAt >= 0 && m_outputRuntime) {
            // outputFrameForPlayheadMs / dispatcherNextOutputFrameIndex lock the
            // output runtime internally; m_outputRuntime itself is worker-thread-owned
            // (reset only on the worker thread; the dtor joins first), so reading the
            // raw pointer here is safe without m_outputRuntimeMutex, as elsewhere on
            // this path. atOut == -1 means the play epoch is not yet established
            // (transient, e.g. just after a prior cut's resetPlayEpoch); during a
            // running rundown the epoch is long settled before any boundary is staged,
            // so we keep the asap-clamp here (fire-at-out-point degrades to as-soon-as-
            // staged in that unreachable window) — the e2e landing gate would surface
            // it if it ever occurred.
            const qint64 atOut = m_outputRuntime->outputFrameForPlayheadMs(fireAt);
            if (atOut > asapFrame) fireFrame = atOut;
        }
        scheduleCutAtFrame(fireFrame, m_armedTargetMs.load());
    }
}

// Store the atomic schedule (output frame index + target ms). Worker thread.
void PlaybackWorker::scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs) {
    m_scheduledCutTargetMs.store(targetMs);
    m_scheduledCutFrame.store(outputFrameIndex);
}

void PlaybackWorker::markStagingCovered() {
    m_stagingCovers.store(true, std::memory_order_release);
#ifdef OLR_GPU_PIPELINE_BUILD
    const auto stagingFence = std::atomic_load_explicit(&m_stagingFence, std::memory_order_acquire);
    if (stagingFence && gpuPipelineEnabled()) {
        m_stagedFenceValue.store(stagingFence->signal(), std::memory_order_release);
    }
#endif
}

bool PlaybackWorker::stagingGpuSurfacesIdle() const {
#ifdef OLR_GPU_PIPELINE_BUILD
    const auto stagingFence = std::atomic_load_explicit(&m_stagingFence, std::memory_order_acquire);
    if (!stagingFence || !gpuPipelineEnabled()) return true;
    const uint64_t target = m_stagedFenceValue.load(std::memory_order_acquire);
    if (target == 0) return true;
    return stagingFence->completedValue() >= target;
#else
    return true;
#endif
}

// Fire the scheduled cut iff the dispatcher's next index has reached it. Called
// from makeOutputSnapshot on the OUTPUT thread, which already holds m_mutex and
// m_bufferMutex in that order (the caller MUST hold both). dispatcherNextIndex was
// read before either worker lock. A successful promotion enters the central commit
// primitive, extending the order through m_outputRuntimeMutex to OutputRuntime::m_mutex.
//
// LOCK ORDER (held: m_mutex -> m_bufferMutex):
//   m_transport->fps()/seek() lock the transport's OWN mutex and release it
//   before emitting posChanged (transport.cpp); posChanged is a QUEUED cross-
//   thread signal to UIManager/controlServer (different threads) so no connected
//   slot runs synchronously here and none re-enters m_bufferMutex. Lock order is
//   therefore strictly m_bufferMutex -> transport::m_mutex, and no path takes
//   transport::m_mutex then m_bufferMutex. No inversion.
PlaybackWorker::PostCommitDispatch
PlaybackWorker::maybeFireScheduledCut(qint64 dispatcherNextIndex) {
    const qint64 scheduled = m_scheduledCutFrame.load();
    if (scheduled < 0) {
        m_scheduledCutDeferredTicks = 0;
        return PostCommitDispatch::None;
    }
    if (!CutSchedule::shouldFireAt(dispatcherNextIndex, scheduled)) return PostCommitDispatch::None;
    if (!m_prerollStagingCache) return PostCommitDispatch::None;
    // GPU staging-swap fence: do not promote staging -> live until the staging
    // decoder has finished writing its GPU surfaces. This is a NON-BLOCKING poll
    // because makeOutputSnapshot holds m_bufferMutex while calling this method;
    // a not-yet-idle staging cache simply defers the cut to the next tick.
    if (!stagingGpuSurfacesIdle()) return PostCommitDispatch::None;
#ifdef OLR_GPU_PIPELINE_BUILD
    if (gpuDeviceLossPending()) {
        const uint64_t recoveryGeneration = GpuDeviceLossMonitor::instance().recordLoss();
        sanitizeCacheForDeviceLossLocked(m_outputCache.get());
        sanitizeCacheForDeviceLossLocked(m_prerollStagingCache.get());
        OutputCommit recoveryCommit;
        recoveryCommit.playheadMs = m_committedPlayheadMs.load(std::memory_order_acquire);
        recoveryCommit.seekGeneration = m_committedGeneration.load(std::memory_order_acquire);
        recoveryCommit.gpuGeneration = recoveryGeneration;
        recoveryCommit.cacheAction = OutputCacheAction::Publish;
        recoveryCommit.coverageMode = OutputCoverageMode::Displayable;
        recoveryCommit.requireCurrentSeek = false;
        recoveryCommit.guardPlayheadCache = true;
        recoveryCommit.dispatch = PostCommitDispatch::Output;
        const OutputCommitResult result = commitOutputStateLocked(recoveryCommit);
        if (!result.committed) return PostCommitDispatch::None;

        m_scheduledCutFrame.store(-1);
        m_stagingCovers.store(false);
        m_decoderFollowMs.store(-1);
        m_forwardCutResyncMs.store(-1, std::memory_order_release);
        m_armedTargetMs.store(-1);
        m_armedFireAtMs.store(-1);
        m_prerollSeekPending.store(false);
        m_stagedFenceValue.store(0, std::memory_order_release);
        m_scheduledCutDeferredTicks = 0;
        m_cutArmed.store(false, std::memory_order_release);
        return result.dispatch;
    }
#endif
    // Manual-seek-vs-in-flight-cut policy: if the operator issued an explicit
    // seekTo after this cut was armed (m_seekGeneration bumped), the seek wins —
    // abort the cut WITHOUT swapping/re-basing so it never snaps to a target the
    // operator has moved away from. The pending seek (m_seekTargetMs) services the
    // jump via the run loop's explicit-seek classify. Clear the armed state so the
    // worker stops staging; the queued re-arm (if any) was already dropped by seekTo.
    if (m_seekGeneration.load(std::memory_order_acquire) !=
        m_armSeekGen.load(std::memory_order_acquire)) {
        // Full disarm (mirror the fire path's cleanup) so the post-abort state is
        // unambiguously "disarmed", not "armed target retained but gated off".
        m_scheduledCutFrame.store(-1);
        m_stagingCovers.store(false);
        m_decoderFollowMs.store(-1);
        m_forwardCutResyncMs.store(-1, std::memory_order_release);
        m_armedTargetMs.store(-1);
        m_armedFireAtMs.store(-1);
        m_prerollSeekPending.store(false);
#ifdef OLR_GPU_PIPELINE_BUILD
        m_stagedFenceValue.store(0, std::memory_order_release);
#endif
        m_scheduledCutDeferredTicks = 0;
        m_cutArmed.store(false, std::memory_order_release);
        return PostCommitDispatch::None;
    }

    const int64_t target = m_scheduledCutTargetMs.load();
    const int64_t newPlayhead =
        CutSchedule::playheadAfterCut(target, dispatcherNextIndex, scheduled, fps());
    const int64_t prePlayhead = m_transport ? m_transport->currentPos() : newPlayhead;
    // Stage the pointer swap, then let the central primitive validate the promoted
    // cache before publication. Rejection restores both owners while snapshots are
    // still excluded by m_bufferMutex.
    std::swap(m_outputCache, m_prerollStagingCache);
    uint64_t gpuGeneration = 0;
#ifdef OLR_GPU_PIPELINE_BUILD
    gpuGeneration = GpuGenerationCounter::instance().current();
#endif
    OutputCommit commit;
    commit.playheadMs = newPlayhead;
    commit.seekGeneration = m_committedGeneration.load(std::memory_order_acquire);
    commit.gpuGeneration = gpuGeneration;
    commit.cacheAction = OutputCacheAction::Publish;
    commit.coverageMode = OutputCoverageMode::Displayable;
    commit.requireCurrentSeek = false;
    commit.guardPlayheadCache = true;
    commit.dispatch = PostCommitDispatch::None;
    OutputCommitResult cutCommit = commitOutputStateLocked(commit);
    if (!cutCommit.committed) {
        // The promoted staging cache is not yet displayable at newPlayhead. Prefer a
        // clean frame by deferring to the next tick, but a SCHEDULED program cut must
        // land on air: after kMaxScheduledCutDeferredTicks deferrals fire it anyway (a
        // brief placeholder or hold-last frame on a lagging feed is acceptable, an
        // indefinitely-deferred cut is not). The forced commit still re-anchors the
        // epoch atomically (F2), so the held frame is emitted at the correct clock.
        if (++m_scheduledCutDeferredTicks < kMaxScheduledCutDeferredTicks) {
            std::swap(m_outputCache, m_prerollStagingCache);
            return PostCommitDispatch::None;
        }
        commit.requireCoverage = false;
        cutCommit = commitOutputStateLocked(commit);
        if (!cutCommit.committed) {
            std::swap(m_outputCache, m_prerollStagingCache);
            return PostCommitDispatch::None;
        }
    }
    m_scheduledCutDeferredTicks = 0;
    // Decoder-follow for a BACKWARD cut: the swap fixed the OUTPUT, but the primary
    // demuxer+decoder bank is still parked AHEAD of the new playhead, so the worker
    // would otherwise hit the reactive backward-jump path (run loop §6.1(2)) one or
    // more passes later. Queue a deterministic resync to the new playhead instead.
    //
    // A FORWARD cut has the inverse problem: the bank is BEHIND the new playhead.
    // Ordinary forward-lag normally skip-forwards, but if P jumps past the bank's
    // newest sample it is indistinguishable from a live tail hold and can spend
    // ticks decoding old frames that trim immediately. Queue a one-shot primary
    // cursor resync that preserves the promoted cache and counts as skipForward,
    // not reposition/cutFollowReposition.
    // Store it BEFORE re-basing the transport playhead: the worker reads the playhead
    // under the transport's mutex (currentPos), which synchronizes-with this release
    // store, so any worker pass that observes the re-based playhead is guaranteed to
    // observe the pending follow too — its follow branch (ordered before the reactive
    // backward-jump) consumes it, and the reactive path never fires (no double
    // reposition). Both resyncs are non-clearing and do NOT bump m_seekGeneration, so
    // the CommitGate never re-engages — no placeholder.
    if (newPlayhead < prePlayhead) {
        m_decoderFollowMs.store(newPlayhead, std::memory_order_release);
        m_forwardCutResyncMs.store(-1, std::memory_order_release);
    } else if (newPlayhead > prePlayhead) {
        m_forwardCutResyncMs.store(newPlayhead, std::memory_order_release);
        m_decoderFollowMs.store(-1, std::memory_order_release);
    }
    // Re-base the playhead WITHOUT bumping m_seekGeneration: m_transport->seek does
    // not touch the worker's seek token, so committedGen stays == seekGen and
    // makeOutputSnapshot exposes the LIVE transport playhead (now == target) against
    // the freshly-published target-covering cache — zero placeholder, no fallback.
    if (m_transport) m_transport->seek(cutCommit.committedPlayheadMs);
    // commitOutputStateLocked already re-anchored the output epoch before opening
    // the unchanged seek-generation gate, so this snapshot cannot combine the
    // promoted cache with the pre-cut clock identity.
    // Click-free audio transition: de-click + drop the stale pre-cut ring so the
    // monitor re-primes the TARGET audio with a fade-in (AudioPlayer::clear does a
    // fade-out + arms a fade-in — no hard pop). The worker's run loop then re-fills
    // m_audioQueue around the new playhead (the stale queued audio is dropped by
    // its dropOlderThan(P)); the OUTPUT-BUS audio is already correct because the
    // promoted staging cache carries the staged target audio (fillStaging Step 1).
    // AudioPlayer::clear is mutex-guarded (safe from this output thread); we do NOT
    // touch m_audioQueue here (it is worker-thread-only).
    if (m_audioPlayer) m_audioPlayer->clear();
    m_stagingCovers.store(false);
    m_scheduledCutFrame.store(-1);
#ifdef OLR_GPU_PIPELINE_BUILD
    m_stagedFenceValue.store(0, std::memory_order_release);
#endif
    m_cutsFired.fetch_add(1, std::memory_order_acq_rel);
    // Clear m_cutArmed LAST: the run loop applies a queued re-arm only once it
    // observes !m_cutArmed, so releasing it after the swap + counter bump above
    // guarantees the worker's next staging fill cannot race this swap.
    m_cutArmed.store(false, std::memory_order_release);
    return cutCommit.dispatch;
}

void PlaybackWorker::run() {
    qDebug() << "Opening file: " << m_currentFilePath;

    if (m_currentFilePath.isEmpty()) return;

    // A stop() issued before/while we get here sets the interruption
    // flag, which (unlike m_running, re-set just below) survives — all
    // loops therefore gate on shouldInterrupt(), not m_running alone.
    m_running = true;

    auto clearDecoders = [this]() {
        shutdownOutputGraph();
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (auto* track : m_decoderBank) {
            track->nativeDecoder.reset(); // Tear down VideoToolbox before freeing track
            if (track->codecCtx) avcodec_free_context(&track->codecCtx);
            delete track;
        }
        m_decoderBank.clear();
        for (auto* aTrack : m_audioDecoderBank) {
            if (aTrack->codecCtx) avcodec_free_context(&aTrack->codecCtx);
            delete aTrack;
        }
        m_audioDecoderBank.clear();
    };

    // --- 1. OPENING & INITIALIZATION (retry until tracks available or stop) ---
    while (!shouldInterrupt()) {
        if (m_fmtCtx) {
            avformat_close_input(&m_fmtCtx);
        }
        clearDecoders();

        AVFormatContext* newCtx = avformat_alloc_context();
        if (!newCtx) {
            msleep(200);
            continue;
        }
        newCtx->interrupt_callback.callback = &PlaybackWorker::ffmpegInterruptCallback;
        newCtx->interrupt_callback.opaque = this;

        if (avformat_open_input(&newCtx, m_currentFilePath.toUtf8().constData(), nullptr, nullptr) <
            0) {
            avformat_close_input(&newCtx);
            msleep(200);
            continue;
        }
        m_fmtCtx = newCtx;
        if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
            avformat_close_input(&m_fmtCtx);
            msleep(200);
            continue;
        }

        int providerIndex = 0;
        for (unsigned int i = 0; i < m_fmtCtx->nb_streams; i++) {
            AVStream* stream = m_fmtCtx->streams[i];
            AVCodecParameters* codecParams = stream->codecpar;

            if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO) {
                // Safety: Don't exceed the number of providers we have in the UI
                if (providerIndex >= m_providers.size()) break;

                // H.264: use hardware NativeVideoDecoder (licensing constraint).
                // MPEG-2 and all other codecs: FFmpeg software decoder.
                const bool isH264 = (codecParams->codec_id == AV_CODEC_ID_H264);
                if (isH264 && queryNativeVideoDecodeCapabilities().h264 &&
                    codecParams->extradata_size >= 8) {
                    // Parse avcC extradata → SPS/PPS NAL payloads (raw, no start codes).
                    // Layout: [0]=0x01 [1..3]=profile/compat/level [4]=0xFF
                    //         [5]=0xE0|numSPS  then numSPS*(2B length + payload)
                    //         then 1B numPPS   then numPPS*(2B length + payload)
                    H26xParameterSets params;
                    bool parseOk = true;
                    const uint8_t* ed = codecParams->extradata;
                    const int edSize = codecParams->extradata_size;
                    int off = 5;
                    const int numSps = off < edSize ? (ed[off] & 0x1f) : 0;
                    off++;
                    for (int s = 0; s < numSps && parseOk; ++s) {
                        if (off + 2 > edSize) {
                            parseOk = false;
                            break;
                        }
                        const int len = (ed[off] << 8) | ed[off + 1];
                        off += 2;
                        if (len <= 0 || off + len > edSize) {
                            parseOk = false;
                            break;
                        }
                        params.h264Sps.append(
                            QByteArray(reinterpret_cast<const char*>(ed + off), len));
                        off += len;
                    }
                    if (parseOk && off < edSize) {
                        const int numPps = ed[off++];
                        for (int p = 0; p < numPps && parseOk; ++p) {
                            if (off + 2 > edSize) {
                                parseOk = false;
                                break;
                            }
                            const int len = (ed[off] << 8) | ed[off + 1];
                            off += 2;
                            if (len <= 0 || off + len > edSize) {
                                parseOk = false;
                                break;
                            }
                            params.h264Pps.append(
                                QByteArray(reinterpret_cast<const char*>(ed + off), len));
                            off += len;
                        }
                    }
                    if (!parseOk || params.h264Sps.isEmpty() || params.h264Pps.isEmpty()) {
                        qWarning() << "PlaybackWorker: H.264 avcC parse failed for stream" << i
                                   << "— skipping track (hardware-only constraint)";
                        continue; // do NOT software-decode H.264
                    }

                    {
                        DecoderTrack* track = new DecoderTrack();
                        track->streamIndex = int(i);
                        track->nativeDecoder = std::make_unique<NativeVideoDecoder>(
                            codecParams->width, codecParams->height);
                        track->h264ParamSets = params;
                        track->codecWidth = codecParams->width;
                        track->codecHeight = codecParams->height;
                        track->provider = m_providers[providerIndex];
                        track->feedIndex = providerIndex;
                        {
                            QMutexLocker bufferLocker(&m_bufferMutex);
                            m_decoderBank.append(track);
                        }
                        providerIndex++;
                        qDebug() << "Worker: Initialized NativeVideoDecoder (H.264) for Stream" << i
                                 << "mapped to Provider" << (providerIndex - 1);
                    }
                    continue;
                }

                // Hardware unavailable or H.264 without usable extradata: skip the track.
                // NEVER software-decode H.264 (hardware-only licensing constraint).
                if (isH264) continue;

                // Non-H.264 codecs (MPEG-2, etc.) fall through to software decode.
                {
                    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
                    if (!codec) continue;

                    AVCodecContext* ctx = avcodec_alloc_context3(codec);
                    if (!ctx) continue;
                    avcodec_parameters_to_context(ctx, codecParams);

                    // Enable Multi-threading for the decoder itself
                    ctx->thread_count = 0;

                    if (avcodec_open2(ctx, codec, nullptr) < 0) {
                        avcodec_free_context(&ctx);
                        continue;
                    }

                    DecoderTrack* track = new DecoderTrack();
                    track->streamIndex = static_cast<int>(i);
                    track->codecCtx = ctx;
                    track->provider = m_providers[providerIndex];
                    track->feedIndex = providerIndex;

                    {
                        QMutexLocker bufferLocker(&m_bufferMutex);
                        m_decoderBank.append(track);
                    }

                    providerIndex++;
                    qDebug() << "Worker: Initialized Decoder for Stream" << i
                             << "mapped to Provider" << (providerIndex - 1);
                }
            }
        }

        // Also detect audio streams (paired with video by order)
        int audioViewIdx = 0;
        for (unsigned int i = 0; i < m_fmtCtx->nb_streams; i++) {
            AVCodecParameters* codecParams = m_fmtCtx->streams[i]->codecpar;
            if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
                const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
                if (!codec) {
                    audioViewIdx++;
                    continue;
                }

                AVCodecContext* ctx = avcodec_alloc_context3(codec);
                if (!ctx) {
                    audioViewIdx++;
                    continue;
                }
                avcodec_parameters_to_context(ctx, codecParams);
                ctx->thread_count = 0;

                if (avcodec_open2(ctx, codec, nullptr) < 0) {
                    avcodec_free_context(&ctx);
                    audioViewIdx++;
                    continue;
                }

                AudioDecoderTrack* aTrack = new AudioDecoderTrack();
                aTrack->streamIndex = static_cast<int>(i);
                aTrack->codecCtx = ctx;
                aTrack->viewIndex = audioViewIdx;
                m_audioDecoderBank.append(aTrack);

                qDebug() << "Worker: Initialized Audio Decoder for Stream" << i << "view"
                         << audioViewIdx;
                audioViewIdx++;
            }
        }

        if (!m_decoderBank.isEmpty()) break;

        qDebug() << "PlaybackWorker: No video tracks yet. Retrying...";
        avformat_close_input(&m_fmtCtx);
        msleep(500);
    }

    if (shouldInterrupt() || m_decoderBank.isEmpty()) {
        clearDecoders();
        if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
        return;
    }

    int outputWidth = 1920;
    int outputHeight = 1080;
    if (!m_decoderBank.isEmpty()) {
        const DecoderTrack* ref = m_decoderBank[0];
        if (ref->codecCtx) {
            outputWidth = qMax(2, ref->codecCtx->width);
            outputHeight = qMax(2, ref->codecCtx->height);
        } else if (ref->codecWidth > 0 && ref->codecHeight > 0) {
            outputWidth = qMax(2, ref->codecWidth);
            outputHeight = qMax(2, ref->codecHeight);
        }
    }
    initializeOutputGraph(static_cast<int>(m_decoderBank.size()), outputWidth, outputHeight);

    // Tier3: open the SECOND (pre-roll) AVFormatContext on the same clip now
    // that the primary bank + output graph are up. On failure the armed-cut
    // feature is silently disabled (armNextCut becomes a no-op).
    if (!openPrerollContext())
        qWarning() << "PlaybackWorker: pre-roll context unavailable — armed cut disabled";

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();

    {
        QMutexLocker locker(&m_mutex);
        m_seekTargetMs = -1;
    }

    // Telemetry: emit a SEC line once per wall-second.
    int64_t lastTelemetryMs = 0;
    QElapsedTimer wallClock;
    wallClock.start();
    // EOF read-error backoff bound (non-EOF errors; §6.8).
    int readErrStreak = 0;
    const int kMaxReadErrStreak = 200;
    int64_t lastLiveEofLogMs = -100000;

    // ----------------------------------------------------------------------
    // THE WINDOWED SCHEDULER (spec §6).
    // ----------------------------------------------------------------------
    while (!shouldInterrupt()) {
#ifdef OLR_GPU_PIPELINE_BUILD
        if (m_gpuRebuildDeferredForSuspend.load(std::memory_order_acquire) &&
            !gpuLifecycleSuspended()) {
            resumeDeferredGpuRebuild();
        }
        const auto gpuRhi = std::atomic_load_explicit(&m_gpuRhi, std::memory_order_acquire);
        if (m_injectGpuDeviceLossForTest.load(std::memory_order_acquire) &&
            gpuPipelineState() == GpuPipelineState::Gpu && gpuRhi) {
            if (m_injectGpuDeviceLossForTest.exchange(false, std::memory_order_acq_rel)) {
                gpuRhi->injectDeviceLostForTest();
                GpuDeviceLossMonitor::instance().recordLoss();
            }
        }
        const bool deviceLossPending = gpuDeviceLossPending();
        const GpuPipelineState pipelineState = gpuPipelineState();
        if ((deviceLossPending && pipelineState != GpuPipelineState::CpuFallback) ||
            pipelineState == GpuPipelineState::RebuildPending) {
            handleGpuDeviceLoss();
        }
        sampleGpuMemoryPressure(wallClock.elapsed());
#endif

        // --- Sample state (spec §6.1) ---
        int64_t P = m_transport->currentPos();
        bool playing = m_transport->isPlaying();
        double speed = m_transport->speed();
        const double aspeed = qAbs(speed);
        int dir = playing ? (speed < 0 ? -1 : 1) : m_lastMoveDir.load(std::memory_order_relaxed);
        const int trackCount = qMax(1, int(m_decoderBank.size()));

        // --- Audio re-prime request from setActiveAudioView (UI thread) ---
        if (m_audioReprime.exchange(false, std::memory_order_relaxed)) {
            m_audioQueue.clear();
            if (m_audioPlayer) m_audioPlayer->clear();
        }
        if (m_outputTargetsDirty.load(std::memory_order_relaxed)) rebuildOutputEndpoints();

        // --- Telemetry: once per wall-second (spec §11.1) ---
        const int64_t nowMs = wallClock.elapsed();
        if (nowMs - lastTelemetryMs >= 1000) {
            lastTelemetryMs = nowMs;
            emitTelemetry(P, newestPtsMax(), speed);
        }

        // --- Pause handling (§6.9): block, but wake on a pending seek OR when
        //     P has left the currently delivered frame's interval. ---
        if (!playing) {
            // Recompute dir for the paused case from the last explicit move.
            dir = m_lastMoveDir.load(std::memory_order_relaxed);
            const bool needWork = pausedPlayheadNeedsWork(P);
            if (!needWork) {
                QMutexLocker locker(&m_mutex);
                if (m_seekTargetMs < 0 && !shouldInterrupt()) m_workerWake.wait(&m_mutex, 10);
                continue;
            }
            // else: fall through to classify→deliver while paused.
        }

        // === CLASSIFY (spec §6.1, priority order) ===

        // (1) Explicit seek — coalesce to the latest target, clear it. → §6.2.
        int64_t seekTarget = -1;
        {
            QMutexLocker locker(&m_mutex);
            if (m_seekTargetMs >= 0) {
                seekTarget = m_seekTargetMs;
                m_seekTargetMs = -1;
            }
        }
        if (seekTarget >= 0) {
            // Anchor direction by the recorded move sign (seekTo sets it in T6).
            int seekDir = m_lastMoveDir.load(std::memory_order_relaxed);
            if (m_outputRuntime) m_outputRuntime->resetPlayEpoch();
            repositionTo(seekTarget, seekDir, pkt, frame, audioFrame);
            continue;
        }

        // (1b) Armed-cut decoder-follow: a BACKWARD cut promoted the target window
        //      into the output cache + re-based the playhead but left the primary
        //      decode engine parked forward. Resync it deterministically HERE,
        //      pre-empting the reactive backward-jump below so exactly one resync
        //      runs. Non-clearing (the promoted cache is preserved); seek
        //      generations untouched (the CommitGate stays disengaged → no gray);
        //      counted as cutFollowReposition, not reposition.
        {
            const int64_t follow = m_decoderFollowMs.exchange(-1, std::memory_order_acquire);
            if (follow >= 0) {
                repositionTo(follow, /*dir*/ -1, pkt, frame, audioFrame, /*cutFollow*/ true);
                continue;
            }
        }

        // (1c) Armed-cut forward primary-bank resync. The output cache was already
        //      promoted at the cut, so this must not clear it or count as a seek
        //      fallback. Wait until this worker pass has observed the re-based
        //      transport playhead; if we race the output thread between token store
        //      and transport->seek, leave the token armed for the next pass.
        {
            const int64_t pendingForwardResync =
                m_forwardCutResyncMs.load(std::memory_order_acquire);
            if (pendingForwardResync >= 0 && P >= pendingForwardResync - frameDurMs()) {
                const int64_t resync = m_forwardCutResyncMs.exchange(-1, std::memory_order_acq_rel);
                if (resync >= 0) {
                    resyncPrimaryDecodeCursorTo(qMax<int64_t>(P, resync));
                }
            }
        }

        // (2) Backward jump: P fell below everything buffered. → §6.2, reverse.
        const int64_t oMin = oldestPtsMin(); // -1 if empty
        if (oMin >= 0 && P < oMin - kBackJumpSlackMs) {
            repositionTo(P, /*dir*/ -1, pkt, frame, audioFrame);
            continue;
        }

        // (3) Forward lag / overrun (playing, dir=+1): decode/playhead is ahead
        //     of what's buffered. NEVER a reposition — skip-forward or tail-hold.
        const int64_t nMin = newestPtsMin(); // -1 if empty
        if (playing && dir == 1 && nMin >= 0 && nMin < P - windowLeadMs()) {
            const int64_t nMax = newestPtsMax();
            if (P > nMax) {
                // §6.8 tail-hold: P is past the written tail. Hold last frame,
                // poll for growth (handled below in the EOF path / idle).
                // Fall through to deliver(last)+wait; no seek.
            } else {
                // §6.5 skip-forward: seek back a trail, resume decimated fill.
                resyncPrimaryDecodeCursorTo(P);
                // Fall through into the fill below (decimated) to repopulate.
            }
        }

        // === (4) DELIVER → FILL → TRIM → AUDIO → WAIT ===

        // --- Deliver the frame at P (direction-aware dedup, §5) ---
        deliverDueFrames(P, dir);

        // --- FILL (direction-aware) ---
        const bool decimate = aspeed > kDecimateAbove;
        const int decStep = decimate ? int(std::ceil(aspeed)) : 1;
        bool hitEof = false;
        bool nonEofErr = false;
        int packetsThisIter = 0;

        if (dir >= 0) {
            // Travelling forward: a fresh reverse run later starts clean.
            m_reverseAnchorMs = INT64_MAX;
            // --- Forward fill (§6.3): bounded to the window + one batch. ---
            const int kFillBatch = 4 * trackCount;
            int batch = 0;
            while (!shouldInterrupt() && batch < kFillBatch) {
                // Stop once the buffered min-newest reaches the lead edge.
                int64_t nm = newestPtsMin();
                if (nm >= 0 && nm >= P + windowLeadMs()) break;
                // Abort fill if a seek arrived.
                {
                    QMutexLocker locker(&m_mutex);
                    if (m_seekTargetMs >= 0) break;
                }

                int ret = readPrimaryFrame(pkt);
                if (ret == AVERROR_EOF) {
                    hitEof = true;
                    break;
                }
                if (ret < 0) {
                    nonEofErr = true;
                    break;
                }
                readErrStreak = 0;
                batch++;
                packetsThisIter++;

                // Audio enqueue only when at 1× forward single-view playing.
                bool audioOn = playing && dir == 1 && (speed > 0.99 && speed < 1.01);
                int64_t lastV = decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/ 1,
                                                     trackCount, decimate, decStep, audioOn,
                                                     /*dedupTail*/ false);
                av_packet_unref(pkt);

                // Terminate when the just-read video packet crosses the slack edge.
                if (lastV != INT64_MIN && lastV > P + windowLeadMs() + windowSlackMs()) break;
            }
        } else {
            // --- Reverse fill (§6.4): fill-then-deliver one chunk atomically. ---
            const int64_t rOldest = refOldestPts();
            const int64_t newAnchor = qMax<int64_t>(0, P - windowLeadMs() - windowChunkMs());
            // Re-fetch only when the window needs filling AND the anchor has
            // descended a full chunk since the last fetch — otherwise
            // consecutive iterations (P drops < kChunkMs apart) re-decode an
            // overlapping window (~2-5x wasted decode under load).
            const bool needFill = (rOldest < 0 || (rOldest > P - windowLeadMs() && P > 0));
            const bool anchorMoved = (m_reverseAnchorMs == INT64_MAX) ||
                                     (m_reverseAnchorMs - newAnchor >= windowChunkMs());
            if (needFill && anchorMoved) {
                m_reverseAnchorMs = newAnchor;
                // Record the avio position of the current oldest (file-position
                // terminator: well-defined under non-interleave skew).
                const int64_t stopPos = (m_fmtCtx->pb) ? avio_tell(m_fmtCtx->pb) : -1;

                AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
                int64_t anchor = newAnchor;
                int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
                av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
                m_counters.reverseChunkSeek++;

                const int kReverseChunkBudget =
                    int(std::ceil(double(windowChunkMs()) /
                                  double(qMax<int64_t>(1, frameDurMs())))) *
                    trackCount * 2;
                int packets = 0;
                while (!shouldInterrupt() && packets < kReverseChunkBudget) {
                    {
                        QMutexLocker locker(&m_mutex);
                        if (m_seekTargetMs >= 0) break;
                    }
                    int ret = readPrimaryFrame(pkt);
                    if (ret == AVERROR_EOF) {
                        hitEof = true;
                        break;
                    }
                    if (ret < 0) {
                        nonEofErr = true;
                        break;
                    }
                    readErrStreak = 0;
                    packets++;
                    packetsThisIter++;

                    // Decode forward WITHOUT delivering partial-chunk frames
                    // (audio muted in reverse). dir=-1 protects [P-kLead, P].
                    decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/ -1, trackCount,
                                         decimate, decStep, /*audioOn*/ false,
                                         /*dedupTail*/ false);

                    // Terminate when the read cursor reaches the previous oldest
                    // file position (the chunk above this is already buffered).
                    int64_t cur = (m_fmtCtx->pb) ? avio_tell(m_fmtCtx->pb) : -1;
                    av_packet_unref(pkt);
                    if (stopPos >= 0 && cur >= 0 && cur >= stopPos) break;
                }
                // After the chunk is filled, top-down reverse delivery resumes.
                deliverDueFrames(P, dir);
            }
        }

        // --- TRIM (§6.6) + audio queue bound ---
        {
            int64_t keepFrom, keepTo;
            if (dir >= 0) {
                keepFrom = P - (windowTrailMs() + windowSlackMs());
                keepTo = P + (windowLeadMs() + windowSlackMs());
            } else {
                keepFrom = P - (windowLeadMs() + windowChunkMs() + windowSlackMs());
                keepTo = P + (windowTrailMs() + windowSlackMs());
            }
            QMutexLocker bufferLocker(&m_bufferMutex);
            for (auto* track : m_decoderBank) {
                TrackBuffer::EvictedFrames evictedTrackFrames;
                track->buffer.trim(keepFrom, keepTo, &evictedTrackFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                collectEvictedGpuFramesLocked(evictedTrackFrames);
#endif
            }
            if (m_outputCache) {
                const qint64 audioKeepFrom = P - windowAudioTrailMs();
                const qint64 keepAudioFromSample =
                    qMax<qint64>(0, audioKeepFrom * qint64(48000) / 1000);
                OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                m_outputCache->trimWindow(keepFrom, keepTo, keepAudioFromSample,
                                          &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif
                publishOutputCacheLocked();
            }
        }
#ifdef OLR_GPU_PIPELINE_BUILD
        drainEvictedGpuFrames();
        flushNativeDecoderPoolsThrottled(nowMs);
#endif
        m_audioQueue.dropOlderThan(P, kAudioLeadMs);

        // --- AUDIO release (§6.7): only at 1× forward, playing, single active
        //     view, unmuted. Release queued frames within kAudioLeadMs of P. ---
        {
            bool oneX = (speed > 0.99 && speed < 1.01);
            int activeView = m_activeAudioView.load(std::memory_order_relaxed);
            bool unmuted = m_audioPlayer && !m_audioPlayer->isMuted();
            if (playing && dir == 1 && oneX && activeView >= 0 && unmuted) {
                AudioFrameQueue::Frame af;
                while (m_audioQueue.releaseDue(P, kAudioLeadMs, af)) {
                    m_audioPlayer->pushSamples(reinterpret_cast<const uint8_t*>(af.pcm.constData()),
                                               static_cast<int>(af.pcm.size()), af.ptsMs, P);
                    m_counters.audioPushes++;
                }
            } else {
                // |speed|≠1 / reverse / multiview / muted: drop queued audio so
                // it can't be stale-re-released on return to 1× (§6.7).
                if (!m_audioQueue.isEmpty()) m_audioQueue.clear();
            }
        }

        // --- Tier3 armed cut: apply a queued re-arm, then pre-roll fill ----
        // A Recall that arrived while the previous cut was in flight queued its
        // target (armNextCut). Now that the cut has fired and cleared m_cutArmed,
        // arm the latest pending target ON THE WORKER THREAD so the subsequent
        // staging fill cannot run concurrently with the output thread's swap.
        if (!m_cutArmed.load(std::memory_order_acquire) &&
            m_hasPendingRearm.exchange(false, std::memory_order_acq_rel)) {
            const uint64_t qgen = m_pendingRearmSeekGen.load(std::memory_order_acquire);
            bool seekPending;
            {
                QMutexLocker l(&m_mutex);
                seekPending = (m_seekTargetMs >= 0);
            }
            // Apply the queued re-arm ONLY if no manual seek has superseded it since
            // it was queued: the generation must be unchanged AND no seek target may
            // be outstanding. Otherwise drop it — the operator's seek is the newer
            // explicit action. armCutInternal is given the QUEUE-time generation as
            // the baseline, so even a seek that races this apply aborts the cut later.
            if (!seekPending && m_seekGeneration.load(std::memory_order_acquire) == qgen)
                armCutInternal(m_pendingRearmMs.load(), qgen,
                               m_pendingRearmFireAtMs.load(std::memory_order_relaxed));
        }
        // Worker-private; never starves the primary tick (kPrerollPacketsPerTick
        // per pass). Stops once staging covers the armed window (m_stagingCovers),
        // at which point the cut is scheduled and fires on the output thread.
        if (m_cutArmed.load() && !m_stagingCovers.load()) fillStaging();

        // --- EOF / live-growth handling (§6.8) ---
        if (hitEof) {
            msleep(kEofSleepMs);
            const int64_t avioSize = (m_fmtCtx && m_fmtCtx->pb) ? avio_size(m_fmtCtx->pb) : -1;
            const QFileInfo inputInfo(m_currentFilePath);
            const int64_t filesystemSize =
                (inputInfo.exists() && inputInfo.isFile()) ? inputInfo.size() : -1;
            int64_t sz = liveGrowthFileSize();
            const int64_t rNewestBefore = refNewestPts();
            const bool shouldLogLiveEof =
                sz > m_sizeAtLastEof || wallClock.elapsed() - lastLiveEofLogMs >= 1000;
            if (shouldLogLiveEof) {
                lastLiveEofLogMs = wallClock.elapsed();
                qDebug() << "PlaybackWorker: live EOF"
                         << "P" << P << "avioSize" << avioSize << "filesystemSize" << filesystemSize
                         << "lastSize" << m_sizeAtLastEof << "refNewest" << rNewestBefore
                         << "newestMin" << newestPtsMin();
            }
            if (sz > m_sizeAtLastEof) {
                // Grown — un-latch and seek back to the reference newest so the
                // matroska demuxer's latched 'done' is cleared.
                if (m_fmtCtx->pb) {
                    m_fmtCtx->pb->eof_reached = 0;
                    m_fmtCtx->pb->error = 0;
                }
                const int64_t rNewest = rNewestBefore;
                AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
                // If playback has outrun the old live tail, the previously-buffered
                // newest frame is no longer a useful recovery anchor: it may already
                // be trimmed, or it may force the demuxer to re-read seconds of stale
                // filler before reaching the newly-grown region. Re-anchor inside the
                // current visible window so live preview catches up immediately after
                // the recorder appends more packets.
                const int64_t recoveryTargetMs = P + windowLeadMs();
                if (rNewest >= recoveryTargetMs) {
                    m_sizeAtLastEof = sz;
                    if (shouldLogLiveEof) {
                        qDebug() << "PlaybackWorker: live EOF already covered"
                                 << "target" << recoveryTargetMs << "refNewest" << rNewest;
                    }
                    continue;
                }
                avformat_flush(m_fmtCtx);
                const int64_t anchorMs =
                    liveEofRecoveryAnchorMs(P, rNewest, windowTrailMs(), frameDurMs());
                int64_t seekPts = av_rescale_q(anchorMs, {1, 1000}, vStream->time_base);
                int sret = av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
                m_counters.eofTailSeek++;
                int drainedPackets = 0;
                int64_t recoveredNewest = rNewest;
                if (sret >= 0) {
                    // Dedup-before-decode: re-read tail clusters cost reads only.
                    // Drain a bounded number of packets, skipping already-buffered.
                    bool audioOn = playing && (speed > 0.99 && speed < 1.01);
                    const int64_t recoverySpanMs =
                        windowTrailMs() + windowLeadMs() + windowChunkMs() + 2000;
                    const int recoveryFrames =
                        int((recoverySpanMs + qMax<int64_t>(1, frameDurMs()) - 1) /
                            qMax<int64_t>(1, frameDurMs()));
                    const int kEofDrain = qMax(4 * trackCount, recoveryFrames * trackCount * 3);
                    for (int i = 0; i < kEofDrain && !shouldInterrupt(); ++i) {
                        const int64_t nm = newestPtsMin();
                        if (nm >= 0 && nm >= recoveryTargetMs) break;
                        int ret = readPrimaryFrame(pkt);
                        if (ret < 0) break;
                        ++drainedPackets;
                        decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/ 1, trackCount,
                                             /*decimate*/ false, /*step*/ 1, audioOn,
                                             /*dedupTail*/ true);
                        av_packet_unref(pkt);
                    }
                    recoveredNewest = newestPtsMin();
                    // Per-insert publish was removed; this path `continue`s past
                    // the run-loop trim. Bound the GPU-backed windows before
                    // publishing recovered frames, otherwise live-growing files
                    // retain every recovered IOSurface until playback stops.
                    {
                        const int64_t keepFrom = P - (windowTrailMs() + windowSlackMs());
                        const int64_t keepTo = P + (windowLeadMs() + windowSlackMs());
                        QMutexLocker bufferLocker(&m_bufferMutex);
                        for (auto* track : m_decoderBank) {
                            TrackBuffer::EvictedFrames evictedTrackFrames;
                            track->buffer.trim(keepFrom, keepTo, &evictedTrackFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                            collectEvictedGpuFramesLocked(evictedTrackFrames);
#endif
                        }
                        if (m_outputCache) {
                            const qint64 audioKeepFrom = P - windowAudioTrailMs();
                            const qint64 keepAudioFromSample =
                                qMax<qint64>(0, audioKeepFrom * qint64(48000) / 1000);
                            OutputFrameCache::EvictedVideoFrames evictedCacheFrames;
                            m_outputCache->trimWindow(keepFrom, keepTo, keepAudioFromSample,
                                                      &evictedCacheFrames);
#ifdef OLR_GPU_PIPELINE_BUILD
                            collectEvictedGpuFramesLocked(evictedCacheFrames);
#endif
                            publishOutputCacheLocked();
                        }
                    }
#ifdef OLR_GPU_PIPELINE_BUILD
                    drainEvictedGpuFrames();
                    flushNativeDecoderPoolsThrottled(wallClock.elapsed());
#endif
                    const bool recoveredTarget = recoveredNewest >= recoveryTargetMs;
                    const bool exhaustedDrainBudget =
                        !recoveredTarget && drainedPackets >= kEofDrain;
                    if (recoveredTarget || !exhaustedDrainBudget) {
                        m_sizeAtLastEof = sz;
                    }
                } else {
                    m_sizeAtLastEof = sz;
                }
                if (shouldLogLiveEof) {
                    qDebug() << "PlaybackWorker: live EOF recovery"
                             << "seekRet" << sret << "anchorMs" << anchorMs << "target"
                             << recoveryTargetMs << "drainedPackets" << drainedPackets
                             << "newestMinAfter" << recoveredNewest << "decodedFrames"
                             << m_counters.decodedVideoFrames << "eofTailSeek"
                             << m_counters.eofTailSeek;
                }
                // If the bounded drain hit its cap before reaching the recovery
                // target, leave m_sizeAtLastEof below sz so the next EOF poll
                // continues the tail instead of freezing on a partial recovery.
            }
            // not grown: just slept; finished file costs only the sleep.
            continue;
        }
        if (nonEofErr) {
            if (++readErrStreak > kMaxReadErrStreak) break; // bounded: stop cleanly
            msleep(kReadErrSleepMs);
            continue;
        }

        // --- WAIT (§6.9): window full / no read this pass → short idle sleep. ---
        // Sleeping when no packet was read also covers tail-hold (§6.8, P past
        // tail) and the bottom-of-file reverse case, preventing a hot spin.
        if (playing) {
            int64_t nm = newestPtsMin();
            bool windowFull = (dir >= 0)
                                  ? (nm >= 0 && nm >= P + windowLeadMs())
                                  : (refOldestPts() >= 0 && refOldestPts() <= P - windowLeadMs());
            if (windowFull || packetsThisIter == 0) msleep(kIdleSleepMs);
        } else {
            // Paused and we did work this pass: brief sleep before re-checking.
            msleep(kIdleSleepMs);
        }
    }

    // --- 6. CLEANUP ---
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&audioFrame);
    clearDecoders();
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);

    // Tier3: free pre-roll resources (worker-thread-owned).
    for (auto* track : m_prerollBank) {
        track->nativeDecoder.reset(); // tear down VT/MF session before freeing track
        if (track->codecCtx) avcodec_free_context(&track->codecCtx);
        delete track;
    }
    m_prerollBank.clear();
    for (auto* aTrack : m_prerollAudioBank) {
        if (aTrack->codecCtx) avcodec_free_context(&aTrack->codecCtx);
        delete aTrack;
    }
    m_prerollAudioBank.clear();
    if (m_prerollFmtCtx) avformat_close_input(&m_prerollFmtCtx);
    m_prerollStagingCache.reset();
    m_cutArmed.store(false);
    m_scheduledCutFrame.store(-1);
    m_stagingCovers.store(false);
#ifdef OLR_GPU_PIPELINE_BUILD
    m_stagedFenceValue.store(0, std::memory_order_release);
#endif
    m_hasPendingRearm.store(false);
    m_decoderFollowMs.store(-1);
    m_forwardCutResyncMs.store(-1);
}

void PlaybackWorker::deliverDueFrames(int64_t P, int dir) {
    struct PendingDeliver {
        FrameProvider* provider = nullptr;
        FrameHandle frame;
    };

    QVector<PendingDeliver> pending;
    const bool outputGraphActive = m_outputRuntime != nullptr;
    const FrameRate rate = m_transport->frameRate();
    const qint64 outputFrameIndex = rate.msToFrameIndex(P);
    {
        QMutexLocker bufferLocker(&m_bufferMutex);

        // Only the inactive-output-graph path needs to render frames here; when
        // the OutputRuntime is active it paints from m_outputCache on its own
        // tick, so building a local snapshot/cache/engine is pure dead work.
        OutputBusEngine* engine = nullptr;
        OutputFrameCache* localCache = nullptr;
        PlaybackStateSnapshot state;
        std::unique_ptr<OutputBusEngine> engineHolder;
        std::unique_ptr<OutputFrameCache> cacheHolder;

        if (!outputGraphActive) {
            int placeholderWidth = 1920;
            int placeholderHeight = 1080;
            QVector<QVector<TrackBuffer::Frame>> snapshots;
            snapshots.reserve(m_decoderBank.size());
            for (auto* track : m_decoderBank) {
                QVector<TrackBuffer::Frame> frames =
                    track ? track->buffer.framesSnapshot() : QVector<TrackBuffer::Frame>();
                for (const TrackBuffer::Frame& frame : frames) {
                    const MediaVideoFrameView view(frame.frame);
                    if (view.isValid()) {
                        placeholderWidth = view.width;
                        placeholderHeight = view.height;
                        break;
                    }
                }
                snapshots.append(frames);
            }

            cacheHolder = std::make_unique<OutputFrameCache>(m_decoderBank.size(), placeholderWidth,
                                                             placeholderHeight);
            for (int trackIndex = 0; trackIndex < m_decoderBank.size(); ++trackIndex) {
                DecoderTrack* track = m_decoderBank[trackIndex];
                if (!track) continue;
                for (TrackBuffer::Frame frame : snapshots[trackIndex]) {
                    frame.frame.metadata().key.feedIndex = track->feedIndex;
                    cacheHolder->insertVideoFrame(frame.frame);
                }
            }
            localCache = cacheHolder.get();

            engineHolder = std::make_unique<OutputBusEngine>(rate, m_decoderBank.size(),
                                                             placeholderWidth, placeholderHeight);
            engine = engineHolder.get();

            state.playheadMs = P;
            state.playing = false;
            state.speed = 1.0;
            state.playStartedAtOutputFrame = outputFrameIndex;
            state.playStartedAtPlayheadMs = P;
            state.selectedFeedIndex = m_decoderBank.isEmpty() ? -1 : 0;
#ifdef OLR_GPU_PIPELINE_BUILD
            if (gpuPipelineEnabled())
                state.gpuGeneration = GpuGenerationCounter::instance().current();
#endif
        }

        for (auto* track : m_decoderBank) {
            if (!track || !track->provider) continue;
            FrameHandle f;
            int64_t p;
            if (track->buffer.frameAt(P, f, p)) {
                // Direction-aware dedup (spec §5): forbid out-of-order paints.
                //  - forward (+1): deliver iff pts moved up (or after a reset);
                //  - reverse (-1): deliver iff pts moved down (or after a reset).
                const int64_t last = track->lastDeliveredPtsMs;
                bool deliver;
                if (last < 0)
                    deliver = true; // post-reset
                else if (dir >= 0)
                    deliver = (p > last);
                else
                    deliver = (p < last);
                if (p == last) deliver = false; // already shown
                if (deliver) {
                    track->lastDeliveredPtsMs = p;
                    if (outputGraphActive) continue;
                    OutputBusFrame busFrame =
                        engine->renderFeed(track->feedIndex, outputFrameIndex, state, *localCache);
                    pending.append({track->provider, busFrame.video});
                }
            }
        }
    }

    for (const auto& item : pending) {
        QtPreviewSink sink(item.provider);
        sink.deliver(item.frame);
    }
}

ColorMetadata colorMetadataForAvFrame(const AVFrame* frame) {
    if (!frame) return defaultColorMetadataForHeight(0);
    return resolveColorMetadata(VuiColorInfo{}, frame->height, int(frame->colorspace),
                                int(frame->color_range), int(frame->color_primaries),
                                int(frame->color_trc));
}

FrameHandle PlaybackWorker::convertToMediaVideoFrame(AVFrame* frame, int feedIndex) {
    // Our recordings are always MPEG-2 all-intra YUV420P. Reject anything else
    // (a foreign MKV, 10-bit or 4:2:2 content) rather than copying it with the
    // wrong plane geometry and rendering garbage. Returns an invalid frame the
    // caller skips.
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 || frame->height <= 0)
        return FrameHandle();

    CpuPlanes out;
    out.format = FramePixelFormat::Yuv420p;
    out.width = frame->width;
    out.height = frame->height;
    out.stride[0] = frame->width;
    out.stride[1] = (frame->width + 1) / 2;
    out.stride[2] = (frame->width + 1) / 2;
    const int chromaH = (frame->height + 1) / 2;
    // Allocate uninitialized: the per-line memcpy below overwrites every byte
    // up to copyW for all `height` lines, so a zero-fill is a dead store
    // (~3 MB memset per 1080p frame). Padding bytes (width..stride) are never
    // read by the renderer.
    out.plane[0] = QByteArray(qsizetype(out.stride[0]) * frame->height, Qt::Uninitialized);
    out.plane[1] = QByteArray(qsizetype(out.stride[1]) * chromaH, Qt::Uninitialized);
    out.plane[2] = QByteArray(qsizetype(out.stride[2]) * chromaH, Qt::Uninitialized);

    const int dstStrides[3] = {out.stride[0], out.stride[1], out.stride[2]};
    for (int i = 0; i < 3; ++i) {
        uint8_t* src = frame->data[i];
        if (!src) return FrameHandle();
        char* dst = out.plane[i].data();
        int srcStride = frame->linesize[i];
        int dstStride = dstStrides[i];
        int height = (i == 0) ? frame->height : (frame->height + 1) / 2;
        int width = (i == 0) ? frame->width : (frame->width + 1) / 2;
        int copyW = qMin(width, qMin(qAbs(srcStride), dstStride));
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcLine = srcStride >= 0
                                         ? (src + qsizetype(y) * srcStride)
                                         : (src + qsizetype(height - 1 - y) * -srcStride);
            memcpy(dst + qsizetype(y) * dstStride, srcLine, size_t(copyW));
        }
    }

    FrameMetadata meta;
    meta.key.feedIndex = feedIndex;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.key.width = frame->width;
    meta.key.height = frame->height;
    meta.stride[0] = out.stride[0];
    meta.stride[1] = out.stride[1];
    meta.stride[2] = out.stride[2];
    meta.color = colorMetadataForAvFrame(frame);
    return makeCpuFrameHandle(std::move(out), meta);
}
