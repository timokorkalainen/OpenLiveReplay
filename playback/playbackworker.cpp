#include "playback/playbackworker.h"
#include "playback/cutschedule.h"
#include "playback/output/broadcastoutputsettings.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputframecache.h"
#include "playback/output/ndisink.h"
#include "playback/output/qtpreviewsink.h"
#include "playback/output/queuedoutputsink.h"
#include <QDebug>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <cstdio>
#include <cmath>
#include <cstdint>

PlaybackWorker::PlaybackWorker(const QList<FrameProvider*>& providers, PlaybackTransport* transport,
                               AudioPlayer* audioPlayer, QObject* parent)
    : QThread(parent) {
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

void PlaybackWorker::seekTo(int64_t timestampMs) {
    const int64_t clamped = qMax<int64_t>(0, timestampMs);
    QMutexLocker locker(&m_mutex);
    // Record travel direction from the current playhead (spec §4/§5/§6.7):
    // drives reverse reposition anchoring, the backward-scrub audio re-prime,
    // and paused dedup direction. m_transport->currentPos() locks the
    // transport's own (independent) mutex, so there is no lock-order concern.
    m_lastMoveDir.store((clamped >= m_transport->currentPos()) ? 1 : -1, std::memory_order_relaxed);
    m_seekTargetMs = clamped;
    // A manual seek supersedes any pending armed-cut decoder-follow: the explicit
    // reposition below resyncs the primary bank to the user's target, so a stale
    // follow to the old cut target would otherwise jump the decoder back. Cancel it.
    m_decoderFollowMs.store(-1, std::memory_order_relaxed);
    // A new seek target is outstanding until repositionTo commits it. The gate
    // holds the last good playhead until m_committedGeneration catches up.
    m_seekGeneration.fetch_add(1, std::memory_order_release);
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) m_outputRuntime->resetPlayEpoch();
    }
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

void PlaybackWorker::setBusPreviewProviders(FrameProvider* multiviewProvider,
                                            FrameProvider* pgmProvider) {
    QMutexLocker locker(&m_mutex);
    m_multiviewPreviewProvider = multiviewProvider;
    m_pgmPreviewProvider = pgmProvider;
    m_outputTargetsDirty.store(true, std::memory_order_relaxed);
}

void PlaybackWorker::setExternalOutputTargets(const QList<OutputTargetAssignment>& assignments) {
    QMutexLocker locker(&m_mutex);
    m_externalOutputAssignments = assignments;
    m_outputTargetsDirty.store(true, std::memory_order_relaxed);
}

OutputDispatchStats PlaybackWorker::outputStats() const {
    QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
    return m_outputRuntime ? m_outputRuntime->stats() : OutputDispatchStats{};
}

void PlaybackWorker::stop() {
    // The interrupt callback (registered on m_fmtCtx) aborts any blocking
    // av_read_frame.  Do NOT poke m_fmtCtx->pb from this thread: the
    // worker owns that object, and writes raced with the EOF handler.
    m_running = false;
    requestInterruption();
    if (QThread::currentThread() != this) {
        wait();
    }
}

int PlaybackWorker::ffmpegInterruptCallback(void* opaque) {
    PlaybackWorker* worker = static_cast<PlaybackWorker*>(opaque);
    return worker ? worker->shouldInterrupt() : 0;
}

bool PlaybackWorker::shouldInterrupt() const {
    return !m_running || isInterruptionRequested();
}

void PlaybackWorker::emitTelemetry(int64_t P, int64_t newest, double speed) {
    if (!qEnvironmentVariableIsSet("OLR_PB_TELEMETRY")) return;
    fprintf(stderr,
            "SEC repos=%d reuse=%d revseek=%d eof=%d skip=%d apush=%d drop=%d P=%lld newest=%lld "
            "spd=%.2f\n",
            m_counters.reposition, m_counters.reuseSeek, m_counters.reverseChunkSeek,
            m_counters.eofTailSeek, m_counters.skipForward, m_counters.audioPushes,
            m_counters.framesDropped, (long long) P, (long long) newest, speed);
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

int PlaybackWorker::capFrames(int trackCount) const {
    // capFrames = clamp( ceil(windowMs / frameDurMs) + 4, 12,
    //                    max(12, kGlobalFrameBudget / max(1,trackCount)) )
    const int64_t windowMs = int64_t(kLeadMs) + kChunkMs + kTrailMs + 2 * int64_t(kSlackMs);
    const int64_t dur = qMax<int64_t>(1, frameDurMs());    // never divide by zero
    const int64_t ceilFrames = (windowMs + dur - 1) / dur; // integer ceil
    int64_t want = ceilFrames + 4;

    const int tc = qMax(1, trackCount);
    const int64_t hi = qMax<int64_t>(12, int64_t(kGlobalFrameBudget) / tc);
    const int64_t lo = 12;

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
        if (n < 0) continue;                   // empty track
        if (n < maxNewest - kLeadMs) continue; // stalled track — exclude
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
        if (n < 0) continue;                   // empty track
        if (n < maxNewest - kLeadMs) continue; // stalled track — exclude
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

void PlaybackWorker::clearDecoderBuffers() {
    QMutexLocker bufferLocker(&m_bufferMutex);
    for (auto* track : m_decoderBank) {
        track->buffer.clear();
        track->decimateCounter = 0;
    }
    // NOTE: deliberately does NOT clear m_outputCache. The OutputRuntime paints
    // exclusively from m_outputCache; wiping it here makes the next ~1ms tick
    // snapshot an empty cache and render the gray placeholder (the seek flash).
    // The cache's stale frames are harmless: the forward fill re-inserts the
    // new frames before the playhead reaches them, and trimBefore() drops the
    // old ones. See docs/superpowers/plans (Tier 1 Task 1).
}

bool PlaybackWorker::reuseAt(int64_t target) {
    // True iff the bank is non-empty AND every track has a decoded frame
    // within frameDurMs/2 of target.
    const int64_t tol = frameDurMs() / 2;
    QMutexLocker bufferLocker(&m_bufferMutex);
    if (m_decoderBank.isEmpty()) return false;
    for (auto* track : m_decoderBank) {
        if (!track->buffer.hasFrameNear(target, tol)) return false;
    }
    return true;
}

void PlaybackWorker::initializeOutputGraph(int feedCount, int width, int height) {
    shutdownOutputGraph();
    m_outputFeedCount = qMax(0, feedCount);
    m_outputWidth = qMax(2, width);
    m_outputHeight = qMax(2, height);
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
            FrameRate::fromFraction(fps(), 1), m_outputFeedCount, m_outputWidth, m_outputHeight);
        m_outputRuntime->setSnapshotProvider([this]() { return makeOutputSnapshot(); });
    }
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
        runtime = std::move(m_outputRuntime);
    }
    if (runtime) {
        runtime->stopRuntime();
        runtime.reset();
    }
    m_outputSinks.clear();
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
        m_outputCache.reset();
        // Drop the published snapshot so a post-teardown tick (if any) falls back
        // to an empty cache rather than holding stale frames from the old graph.
        m_publishedCache.publish(nullptr);
    }
    m_outputFeedCount = 0;
}

void PlaybackWorker::rebuildOutputEndpoints() {
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return;
    }

    QList<OutputTargetAssignment> external;
    FrameProvider* multiviewProvider = nullptr;
    FrameProvider* pgmProvider = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        external = m_externalOutputAssignments;
        multiviewProvider = m_multiviewPreviewProvider;
        pgmProvider = m_pgmPreviewProvider;
    }

    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (!m_outputRuntime) return;
        m_outputRuntime->setEndpoints({});
    }
    m_outputSinks.clear();
    QList<OutputEndpoint> endpoints;

    const QList<OutputTargetAssignment> previews = BroadcastOutputSettings::qtPreviewAssignments(
        m_outputFeedCount, multiviewProvider != nullptr, pgmProvider != nullptr);
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

        auto sink = std::make_unique<QtPreviewOutputSink>(provider);
        endpoints.append({preview, sink.get()});
        m_outputSinks.push_back(std::move(sink));
    }

    for (const OutputTargetAssignment& assignment : external) {
        if (!assignment.enabled) continue;

        std::unique_ptr<IOutputSink> sink;
        switch (assignment.kind) {
        case OutputTargetKind::Ndi:
            sink = std::make_unique<QueuedOutputSink>(std::make_unique<NdiOutputSink>());
            break;
        case OutputTargetKind::QtPreview:
        case OutputTargetKind::DeckLinkSdiHdmi:
        case OutputTargetKind::DeckLinkIpSt2110:
        case OutputTargetKind::Omt:
        case OutputTargetKind::Aja:
            break;
        }
        if (!sink) continue;
        endpoints.append({assignment, sink.get()});
        m_outputSinks.push_back(std::move(sink));
    }

    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) m_outputRuntime->setEndpoints(endpoints);
    }
    m_outputTargetsDirty.store(false, std::memory_order_relaxed);
}

void PlaybackWorker::publishOutputCacheLocked() {
    if (m_outputCache)
        m_publishedCache.publish(std::make_shared<const OutputFrameCache>(*m_outputCache));
}

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
    {
        // Tier 2: read the immutable published snapshot instead of deep-copying
        // the live m_outputCache on every ~1ms tick. The slot's load() takes one
        // short lock and returns a shared_ptr to a const cache the worker never
        // mutates again, so the assignment below copies an implicitly-shared
        // (cheap COW) snapshot, not a re-decode of the decoder track buffers.
        QMutexLocker bufferLocker(&m_bufferMutex);
        // Fire the scheduled cut (if due) while holding m_bufferMutex, BEFORE
        // reading the published cache so this tick paints the promoted window.
        const_cast<PlaybackWorker*>(this)->maybeFireScheduledCut(dispatcherNextIndex);
        if (auto published = m_publishedCache.load())
            snapshot.cache = *published;
        else
            snapshot.cache = OutputFrameCache(m_outputFeedCount, m_outputWidth, m_outputHeight);
    }

    // Gate the visible playhead behind the committed cache generation: when no
    // seek is outstanding (committedGen == seekGen) this returns the LIVE
    // transport playhead so 1x playback advances every tick; while a reposition
    // for the latest seek is in flight it holds the last committed playhead so
    // the snapshot never reports a new playhead against a not-yet-ready cache.
    const qint64 transportPlayhead = m_transport ? m_transport->currentPos() : 0;
    snapshot.state.playheadMs = CommitGate::visiblePlayheadMs(
        transportPlayhead, m_committedPlayheadMs.load(std::memory_order_acquire),
        m_committedGeneration.load(std::memory_order_acquire),
        m_seekGeneration.load(std::memory_order_acquire));
    snapshot.state.playing = m_transport && m_transport->isPlaying();
    snapshot.state.speed = m_transport ? m_transport->speed() : 1.0;
    snapshot.state.selectedFeedIndex = m_selectedOutputFeed.load(std::memory_order_relaxed);
    if (snapshot.state.selectedFeedIndex < 0 && m_outputFeedCount > 0)
        snapshot.state.selectedFeedIndex = 0;
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
    const int cap = capFrames(trackCount);
    // Protect the active fill range in the travel direction (spec §6.6) so the
    // cap can never evict a frame the window still needs:
    //   forward: [P, P + kLeadMs]   reverse: [P - kLeadMs, P]
    const int64_t protectLo = (dir >= 0) ? P : (P - kLeadMs);
    const int64_t protectHi = (dir >= 0) ? (P + kLeadMs) : P;

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
                const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                                      | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
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
                unit.pts90k = (pktPts != AV_NOPTS_VALUE)
                    ? av_rescale_q(pktPts, tb, {1, 90000}) : -1;
                unit.dts90k = (pkt->dts != AV_NOPTS_VALUE)
                    ? av_rescale_q(pkt->dts, tb, {1, 90000}) : -1;
                unit.annexB = annexB;

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
                    int64_t framePtsMs;
                    if (pktPts != AV_NOPTS_VALUE) {
                        framePtsMs = av_rescale_q(pktPts, tb, {1, 1000});
                    } else {
                        framePtsMs = (lastVideoPtsMs != INT64_MIN) ? lastVideoPtsMs + frameDurMs() : P;
                    }

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

                    MediaVideoFrame mediaFrame = convertToMediaVideoFrame(nativeVf, track->feedIndex);
                    mediaFrame.ptsMs = framePtsMs;
                    if (mediaFrame.isValid()) {
                        QMutexLocker bufferLocker(&m_bufferMutex);
                        if (!track->buffer.insert(framePtsMs, mediaFrame, cap, protectLo, protectHi))
                            m_counters.framesDropped++;
                        if (m_outputCache) m_outputCache->insertVideoFrame(mediaFrame);
                    }
                    lastVideoPtsMs = framePtsMs;
                    av_frame_free(&nativeVf);
                };

                // All-intra invariant: each access unit decodes to exactly one
                // keyframe, so handleFrame is called once per unit and the
                // per-frame decimateCounter/lastVideoPtsMs advancement matches
                // the FFmpeg path's per-receive_frame loop.
                track->nativeDecoder->decode(unit, handleFrame, nullptr);
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

                // Tier 3: index the primary video stream's PTS->byte-offset so a
                // later full reposition can avio_seek straight to the exact
                // packet (ALL-INTRA ⇒ any offset is a valid decode start). Keys
                // off the SAME stream the reposition seek uses (m_decoderBank[0]).
                // append() keeps PTS strictly increasing, so re-decoded regions
                // are harmlessly ignored and the index stays sorted.
                if (!m_decoderBank.isEmpty() &&
                    track->streamIndex == m_decoderBank[0]->streamIndex && pkt->pos >= 0) {
                    m_frameIndex.append(framePtsMs, static_cast<qint64>(pkt->pos));
                }

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

                MediaVideoFrame mediaFrame = convertToMediaVideoFrame(vf, track->feedIndex);
                mediaFrame.ptsMs = framePtsMs;
                if (mediaFrame.isValid()) {
                    QMutexLocker bufferLocker(&m_bufferMutex);
                    if (!track->buffer.insert(framePtsMs, mediaFrame, cap, protectLo, protectHi))
                        m_counters.framesDropped++;
                    // Insert only; republish is batched (run-loop trim /
                    // reposition merge), never per-frame — see enqueue note.
                    if (m_outputCache) m_outputCache->insertVideoFrame(mediaFrame);
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
    const int trackCount = qMax(1, int(m_decoderBank.size()));

    // --- Reuse fast-path: every track already has a real frame at target. ---
    if (reuseAt(target)) {
        resetDedup();
        deliverDueFrames(target, dir);
        // Backward reuse-seek is an audio reposition (§6.7): clear + re-prime,
        // never a silent re-release (AudioPlayer's overlap-trim would swallow it).
        if (dir < 0) {
            m_audioQueue.clear();
            if (m_audioPlayer) m_audioPlayer->clear();
        }
        m_counters.reuseSeek++;
        // Tier 2: the cache already covers `target` (reuseAt + deliverDueFrames
        // above). Record the committed playhead, then advance the committed
        // generation to the latest seek's value so makeOutputSnapshot exposes
        // the live transport playhead again (CommitGate).
        m_committedPlayheadMs.store(target, std::memory_order_relaxed);
        m_committedGeneration.store(m_seekGeneration.load(std::memory_order_acquire),
                                    std::memory_order_release);
        // Re-anchor the output clock to the now-live playhead AFTER the commit.
        // seekTo's resetPlayEpoch (issued before this reposition) can be consumed
        // by an output tick while the CommitGate is still holding the OLD
        // playhead, anchoring the play epoch to it; without re-anchoring here the
        // epoch stays at the old position, sampledPlayheadMs diverges by the seek
        // distance, and the output samples a window the cache never covers -> a
        // gray placeholder with no last-good frame (the farback flake).
        {
            QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
            if (m_outputRuntime) m_outputRuntime->resetPlayEpoch();
        }
        return;
    }

    // --- Full reposition: clear everything, seek behind target, fill forward. ---
    clearDecoderBuffers();
    m_reverseAnchorMs = INT64_MAX; // a seek invalidates the reverse-fetch run
    m_audioQueue.clear();
    for (auto* aTrack : m_audioDecoderBank) {
        aTrack->lastEnqueuedPtsMs = -1;
        aTrack->lastCachedPtsMs = -1;
    }
    if (m_audioPlayer) m_audioPlayer->clear();

    const int64_t anchor = qMax<int64_t>(0, target - (dir < 0 ? kLeadMs : kTrailMs));

    const int primaryVideoStreamIndex = m_decoderBank[0]->streamIndex;
    AVStream* vStream = m_fmtCtx->streams[primaryVideoStreamIndex];

    // Tier 3 exact-offset seek: if the primary-stream FrameIndex has an entry
    // at/just-before the coarse anchor, avio_seek straight to that byte offset
    // instead of the byte-coarse av_seek_frame BACKWARD. ALL-INTRA ⇒ any indexed
    // offset is a valid standalone decode start, so the forward fill below covers
    // the SAME [anchor .. target+lead] window (trail intact — back-step reuse and
    // trimBefore both depend on it) while decoding fewer pre-anchor frames. We
    // index off the anchor (not target) so the trail below target is still
    // populated exactly as the coarse path would.
    //
    // VALIDATION: a raw byte avio_seek into the middle of a non-interleaved /
    // multi-track Matroska is NOT guaranteed to resync to that PTS — the demuxer
    // can land a long way off, which would shorten the trail and storm the seek
    // path. So we PROBE the landed primary-video PTS and only keep the exact seek
    // when it lands in the useful band [anchor - kTrailMs, target]; otherwise we
    // fall back to the proven coarse av_seek_frame BACKWARD. Fully additive: when
    // the region is unindexed, pb is not byte-seekable, the seek fails, or the
    // probe lands out of band, we behave exactly like before — no gate regresses.
    bool exactSought = false;
    if (m_fmtCtx->pb) {
        const std::optional<qint64> offset = m_frameIndex.nearestAtOrBefore(anchor);
        if (offset.has_value() && avio_seek(m_fmtCtx->pb, offset.value(), SEEK_SET) >= 0) {
            avformat_flush(m_fmtCtx);
            // Probe forward for the first primary-video packet and read its PTS
            // without decoding/inserting. Accept only if it landed in-band.
            const int64_t bandLo = qMax<int64_t>(0, anchor - kTrailMs);
            int probed = 0;
            bool landedInBand = false;
            while (probed++ < 64 && av_read_frame(m_fmtCtx, pkt) >= 0) {
                if (pkt->stream_index == primaryVideoStreamIndex) {
                    int64_t pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
                    if (pts != AV_NOPTS_VALUE) {
                        const int64_t ptsMs = av_rescale_q(pts, vStream->time_base, {1, 1000});
                        landedInBand = (ptsMs >= bandLo && ptsMs <= target);
                    }
                    av_packet_unref(pkt);
                    break;
                }
                av_packet_unref(pkt);
            }
            if (landedInBand && avio_seek(m_fmtCtx->pb, offset.value(), SEEK_SET) >= 0) {
                avformat_flush(m_fmtCtx); // rewind to the clean offset for the fill
                exactSought = true;
            }
        }
    }
    if (!exactSought) {
        int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
        av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
    }
    // Drain the decoders' OUTPUT queues. Intra-only means the next packet decodes
    // standalone (no reference-frame priming needed), but a seek does NOT discard
    // frames already DECODED and queued before it: on a BACKWARD seek the first
    // avcodec_receive_frame would otherwise return a stale frame from the old
    // (forward) position, whose PTS trips the `newestPtsMin() >= fillTo` break
    // below so the fill aborts with the bank still parked forward — the reposition
    // then has to be retried (2-3 reactive backward-jumps) before the stale frames
    // drain. Flushing here makes a single reposition resync the bank deterministically.
    for (auto* track : m_decoderBank)
        if (track->codecCtx) avcodec_flush_buffers(track->codecCtx);
    for (auto* aTrack : m_audioDecoderBank)
        if (aTrack->codecCtx) avcodec_flush_buffers(aTrack->codecCtx);

    // Decode forward through target + frameDurMs, inserting all tracks. Audio is
    // re-primed by the normal forward release after the reposition, so we do NOT
    // enqueue here (audio queue stays empty until forward fill repopulates it).
    const int64_t fillTo = target + frameDurMs();
    int packets = 0;
    const int packetBudget = (capFrames(trackCount) + 4) * trackCount * 2;
    bool deliveredEarly = false;

    // Tier 2 double-buffer: decode the target window into a fresh staging cache,
    // then merge it into the live cache and trim old frames only AFTER coverage,
    // so the live cache is never momentarily empty at target during a far seek.
    if (m_outputFeedCount > 0) {
        if (!m_stagingCache)
            m_stagingCache = std::make_unique<OutputFrameCache>(m_outputFeedCount, m_outputWidth,
                                                                m_outputHeight);
        else
            m_stagingCache->clear();
    }
    // Swap so decodePacketIntoBank's m_outputCache->insertVideoFrame lands in
    // staging, not the live cache (which keeps its old frames over the seek).
    std::unique_ptr<OutputFrameCache> liveSaved;
    if (m_stagingCache) {
        QMutexLocker bufferLocker(&m_bufferMutex);
        liveSaved = std::move(m_outputCache);
        m_outputCache = std::move(m_stagingCache);
    }

    while (!shouldInterrupt()) {
        // A newer explicit seek supersedes this fill.
        {
            QMutexLocker locker(&m_mutex);
            if (m_seekTargetMs >= 0) break;
        }

        int ret = av_read_frame(m_fmtCtx, pkt);
        if (ret < 0) break; // EOF/short file: deliver what we have

        // Reposition decodes forward from the anchor; protect the [target,
        // target+kLead] span (dir=+1) — the trail below target is also kept by
        // the anchor being kTrailMs/kLeadMs below it.
        decodePacketIntoBank(pkt, vf, af, target, /*dir*/ 1, trackCount,
                             /*decimate*/ false, /*step*/ 1,
                             /*audioOn*/ false, /*dedupTail*/ false);
        av_packet_unref(pkt);

        // All-intra deliver-first: the moment every track has a frame at the
        // target, paint it — do NOT wait for the whole trail/lead to fill. The
        // loop keeps filling afterwards (same inserts → back-step reuse intact);
        // the final deliverDueFrames below is a dedup no-op for this pts.
        if (!deliveredEarly && reuseAt(target)) {
            resetDedup();
            deliverDueFrames(target, dir);
            deliveredEarly = true;
        }

        if (++packets > packetBudget) break; // safety bound
        if (newestPtsMin() >= fillTo) break; // covered the target
    }

    // Merge staging into the live cache, then drop old frames before target.
    if (liveSaved) {
        QMutexLocker bufferLocker(&m_bufferMutex);
        m_stagingCache = std::move(m_outputCache); // staging back
        m_outputCache = std::move(liveSaved);      // live restored (old frames intact)
        m_outputCache->mergeFrom(*m_stagingCache); // live now covers target AND keeps old
        const qint64 keepAudioFromSample =
            qMax<qint64>(0, (target - kLeadMs) * qint64(48000) / 1000);
        m_outputCache->trimBefore(target - kLeadMs, keepAudioFromSample);
        publishOutputCacheLocked();
    }

    if (!deliveredEarly) resetDedup();
    deliverDueFrames(target, dir);
    // An armed-cut decoder-follow resync counts separately: the output cache was
    // already promoted/correct by the cut, so this is NOT a coarse-seek fallback
    // (which the reposition counter / armed-cut gate guard against).
    if (cutFollow)
        m_counters.cutFollowReposition++;
    else
        m_counters.reposition++;

    // Tier 2: the cache now covers `target`. Record the committed playhead
    // first, then advance the committed generation to the latest seek's value
    // — once these match m_seekGeneration, makeOutputSnapshot exposes the live
    // transport playhead again (CommitGate).
    m_committedPlayheadMs.store(target, std::memory_order_relaxed);
    m_committedGeneration.store(m_seekGeneration.load(std::memory_order_acquire),
                                std::memory_order_release);
    // Re-anchor the output clock to the now-live playhead AFTER the commit (see
    // the reuse fast-path above): otherwise the play epoch stays anchored to the
    // CommitGate-held OLD playhead, sampledPlayheadMs diverges by the seek
    // distance, and the output grays a window the cache never covers.
    {
        QMutexLocker runtimeLocker(&m_outputRuntimeMutex);
        if (m_outputRuntime) m_outputRuntime->resetPlayEpoch();
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
        track->streamIndex = i;
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
        aTrack->streamIndex = i;
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
// to targetMs. No-op (feature unavailable) if the pre-roll context failed to
// open. v1 is single-clip (ms-only; same currently-open file).
void PlaybackWorker::armNextCut(int64_t targetMs) {
    if (!m_prerollFmtCtx) return; // pre-roll disabled — feature unavailable
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
        m_hasPendingRearm.store(true, std::memory_order_release);
        return;
    }
    armCutInternal(targetMs);
}

// Arm the cut state. Caller guarantees no cut is in flight (m_cutArmed false), so
// the subsequent worker-thread fillStaging never races the output swap. Atomics
// only (no cache touch) — safe from the UI thread (fresh arm) or the worker
// thread (queued re-arm applied in the run loop). m_cutArmed is released LAST so
// a worker observing it true (acquire) sees the target/seek-pending stores.
void PlaybackWorker::armCutInternal(int64_t targetMs) {
    m_armedTargetMs.store(targetMs < 0 ? 0 : targetMs);
    m_prerollSeekPending.store(true);
    m_stagingCovers.store(false);
    m_scheduledCutFrame.store(-1);
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
        const int64_t anchor = qMax<int64_t>(0, target - kTrailMs);
        const int64_t seekPts = av_rescale_q(anchor, {1, 1000}, refStream->time_base);
        av_seek_frame(m_prerollFmtCtx, refStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
        avformat_flush(m_prerollFmtCtx);
        m_prerollStagingCache->clear();
        m_stagingNewestRefPtsMs = INT64_MIN;
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
            m_stagingCovers.store(true);
            av_packet_unref(pkt);
            break;
        }

        // Decode video packets into the staging cache (mirror decodePacketIntoBank's
        // video insert path, but into m_prerollStagingCache; no FrameIndex append,
        // no per-track buffer, no live cache touch).
        for (auto* track : m_prerollBank) {
            if (pkt->stream_index != track->streamIndex) continue;
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
                    MediaVideoFrame mediaFrame = convertToMediaVideoFrame(vf, track->feedIndex);
                    mediaFrame.ptsMs = framePtsMs;
                    if (mediaFrame.isValid()) {
                        m_prerollStagingCache->insertVideoFrame(mediaFrame);
                        if (track->streamIndex == primaryStreamIndex)
                            m_stagingNewestRefPtsMs = qMax(m_stagingNewestRefPtsMs, framePtsMs);
                    }
                    av_frame_unref(vf);
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
            m_stagingCovers.store(true);
            break;
        }
    }

    av_frame_free(&vf);
    av_frame_free(&af);
    av_packet_free(&pkt);

    // Schedule the cut the moment staging first covers the target window.
    if (m_stagingCovers.load() && m_scheduledCutFrame.load() < 0) {
        const qint64 lead = CutSchedule::framesForLeadMs(kCutLeadMs, fps());
        const qint64 nextIdx =
            m_outputRuntime ? m_outputRuntime->dispatcherNextOutputFrameIndex() : 0;
        scheduleCutAtFrame(CutSchedule::outputFrameForCut(nextIdx, static_cast<int>(lead)),
                           m_armedTargetMs.load());
    }
}

// Store the atomic schedule (output frame index + target ms). Worker thread.
void PlaybackWorker::scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs) {
    m_scheduledCutTargetMs.store(targetMs);
    m_scheduledCutFrame.store(outputFrameIndex);
}

// Fire the scheduled cut iff the dispatcher's next index has reached it. Called
// from makeOutputSnapshot on the OUTPUT thread, which already holds m_bufferMutex
// (the caller MUST hold it). dispatcherNextIndex was read by the caller BEFORE
// locking m_bufferMutex (see makeOutputSnapshot) so this never locks the output
// runtime's m_mutex while holding m_bufferMutex.
//
// LOCK ORDER (held: m_bufferMutex):
//   m_transport->fps()/seek() lock the transport's OWN mutex and release it
//   before emitting posChanged (transport.cpp); posChanged is a QUEUED cross-
//   thread signal to UIManager/controlServer (different threads) so no connected
//   slot runs synchronously here and none re-enters m_bufferMutex. Lock order is
//   therefore strictly m_bufferMutex -> transport::m_mutex, and no path takes
//   transport::m_mutex then m_bufferMutex. No inversion.
void PlaybackWorker::maybeFireScheduledCut(qint64 dispatcherNextIndex) {
    const qint64 scheduled = m_scheduledCutFrame.load();
    if (scheduled < 0) return; // nothing scheduled — unarmed path unchanged
    if (!CutSchedule::shouldFireAt(dispatcherNextIndex, scheduled)) return;
    if (!m_prerollStagingCache) return;

    const int64_t target = m_scheduledCutTargetMs.load();
    const int64_t newPlayhead =
        CutSchedule::playheadAfterCut(target, dispatcherNextIndex, scheduled, fps());
    const int64_t prePlayhead = m_transport ? m_transport->currentPos() : newPlayhead;
    // Atomic promotion: a single pointer swap of the published double-buffer.
    std::swap(m_outputCache, m_prerollStagingCache);
    publishOutputCacheLocked();
    // Decoder-follow for a BACKWARD cut: the swap fixed the OUTPUT, but the primary
    // demuxer+decoder bank is still parked AHEAD of the new playhead, so the worker
    // would otherwise hit the reactive backward-jump path (run loop §6.1(2)) one or
    // more passes later. Queue a deterministic resync to the new playhead instead.
    // Store it BEFORE re-basing the transport playhead: the worker reads the playhead
    // under the transport's mutex (currentPos), which synchronizes-with this release
    // store, so any worker pass that observes the re-based playhead is guaranteed to
    // observe the pending follow too — its follow branch (ordered before the reactive
    // backward-jump) consumes it, and the reactive path never fires (no double
    // reposition). A FORWARD cut leaves m_decoderFollowMs unset: the forward-lag
    // skip-forward path resyncs the bank with no reposition (forward cut stays
    // reposition==0). The follow is non-clearing (the promoted cache is preserved via
    // repositionTo's staging double-buffer) and does NOT bump m_seekGeneration, so the
    // CommitGate never re-engages — no placeholder.
    if (newPlayhead < prePlayhead) m_decoderFollowMs.store(newPlayhead, std::memory_order_release);
    // Re-base the playhead WITHOUT bumping m_seekGeneration: m_transport->seek does
    // not touch the worker's seek token, so committedGen stays == seekGen and
    // makeOutputSnapshot exposes the LIVE transport playhead (now == target) against
    // the freshly-published target-covering cache — zero placeholder, no fallback.
    if (m_transport) m_transport->seek(newPlayhead);
    // Re-anchor the output clock to the new (target) playhead. Without this the
    // dispatcher's play epoch stays anchored to the PRE-CUT play start, so
    // sampledPlayheadMs (which drives the cache lookup) diverges from the target
    // by the cut distance and the output renders the WRONG frame — the cut is not
    // frame-accurate even though it reports zero placeholder/reposition. (Same
    // class of bug as the reposition-commit re-anchor.) resetPlayEpoch locks the
    // output runtime's OWN mutex (independent of the m_bufferMutex held here), and
    // OutputRuntime::snapshot() invokes this provider OUTSIDE that mutex, so there
    // is no re-entrancy or lock-order inversion.
    if (m_outputRuntime) m_outputRuntime->resetPlayEpoch();
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
    m_cutsFired.fetch_add(1, std::memory_order_acq_rel);
    // Clear m_cutArmed LAST: the run loop applies a queued re-arm only once it
    // observes !m_cutArmed, so releasing it after the swap + counter bump above
    // guarantees the worker's next staging fill cannot race this swap.
    m_cutArmed.store(false, std::memory_order_release);
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
                if (isH264 && queryNativeVideoDecodeCapabilities().h264
                    && codecParams->extradata_size >= 8) {
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
                        if (off + 2 > edSize) { parseOk = false; break; }
                        const int len = (ed[off] << 8) | ed[off + 1];
                        off += 2;
                        if (len <= 0 || off + len > edSize) { parseOk = false; break; }
                        params.h264Sps.append(
                            QByteArray(reinterpret_cast<const char*>(ed + off), len));
                        off += len;
                    }
                    if (parseOk && off < edSize) {
                        const int numPps = ed[off++];
                        for (int p = 0; p < numPps && parseOk; ++p) {
                            if (off + 2 > edSize) { parseOk = false; break; }
                            const int len = (ed[off] << 8) | ed[off + 1];
                            off += 2;
                            if (len <= 0 || off + len > edSize) { parseOk = false; break; }
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
                        qDebug() << "Worker: Initialized NativeVideoDecoder (H.264) for Stream"
                                 << i << "mapped to Provider" << (providerIndex - 1);
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
                track->streamIndex = i;
                track->codecCtx = ctx;
                track->provider = m_providers[providerIndex];
                track->feedIndex = providerIndex;

                {
                    QMutexLocker bufferLocker(&m_bufferMutex);
                    m_decoderBank.append(track);
                }

                providerIndex++;
                qDebug() << "Worker: Initialized Decoder for Stream" << i << "mapped to Provider"
                         << (providerIndex - 1);
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
                aTrack->streamIndex = i;
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
    initializeOutputGraph(m_decoderBank.size(), outputWidth, outputHeight);

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

    // ----------------------------------------------------------------------
    // THE WINDOWED SCHEDULER (spec §6).
    // ----------------------------------------------------------------------
    while (!shouldInterrupt()) {

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
            // Has the playhead left the delivered frame's interval? If the
            // reference track has no frame at P, we must (re)deliver/reposition.
            bool needWork = false;
            {
                QMutexLocker locker(&m_mutex);
                if (m_seekTargetMs >= 0) needWork = true;
            }
            if (!needWork) {
                // If a frame at P exists and is already the delivered one, idle.
                QMutexLocker bufferLocker(&m_bufferMutex);
                if (!m_decoderBank.isEmpty()) {
                    MediaVideoFrame f;
                    int64_t p = -1;
                    DecoderTrack* ref = m_decoderBank[0];
                    if (!ref->buffer.frameAt(P, f, p) || p != ref->lastDeliveredPtsMs)
                        needWork = true;
                } else {
                    needWork = true;
                }
            }
            if (!needWork) {
                msleep(10);
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

        // (2) Backward jump: P fell below everything buffered. → §6.2, reverse.
        const int64_t oMin = oldestPtsMin(); // -1 if empty
        if (oMin >= 0 && P < oMin - kBackJumpSlackMs) {
            repositionTo(P, /*dir*/ -1, pkt, frame, audioFrame);
            continue;
        }

        // (3) Forward lag / overrun (playing, dir=+1): decode/playhead is ahead
        //     of what's buffered. NEVER a reposition — skip-forward or tail-hold.
        const int64_t nMin = newestPtsMin(); // -1 if empty
        if (playing && dir == 1 && nMin >= 0 && nMin < P - kLeadMs) {
            const int64_t nMax = newestPtsMax();
            if (P > nMax) {
                // §6.8 tail-hold: P is past the written tail. Hold last frame,
                // poll for growth (handled below in the EOF path / idle).
                // Fall through to deliver(last)+wait; no seek.
            } else {
                // §6.5 skip-forward: seek back a trail, resume decimated fill.
                AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
                int64_t anchor = qMax<int64_t>(0, P - kTrailMs);
                int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
                av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
                clearDecoderBuffers();
                m_audioQueue.clear();
                for (auto* aTrack : m_audioDecoderBank) {
                    aTrack->lastEnqueuedPtsMs = -1;
                    aTrack->lastCachedPtsMs = -1;
                }
                if (m_audioPlayer) m_audioPlayer->clear();
                m_counters.skipForward++;
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
                if (nm >= 0 && nm >= P + kLeadMs) break;
                // Abort fill if a seek arrived.
                {
                    QMutexLocker locker(&m_mutex);
                    if (m_seekTargetMs >= 0) break;
                }

                int ret = av_read_frame(m_fmtCtx, pkt);
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
                if (lastV != INT64_MIN && lastV > P + kLeadMs + kSlackMs) break;
            }
        } else {
            // --- Reverse fill (§6.4): fill-then-deliver one chunk atomically. ---
            const int64_t rOldest = refOldestPts();
            const int64_t newAnchor = qMax<int64_t>(0, P - kLeadMs - kChunkMs);
            // Re-fetch only when the window needs filling AND the anchor has
            // descended a full chunk since the last fetch — otherwise
            // consecutive iterations (P drops < kChunkMs apart) re-decode an
            // overlapping window (~2-5x wasted decode under load).
            const bool needFill = (rOldest < 0 || (rOldest > P - kLeadMs && P > 0));
            const bool anchorMoved =
                (m_reverseAnchorMs == INT64_MAX) || (m_reverseAnchorMs - newAnchor >= kChunkMs);
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
                    int(std::ceil(double(kChunkMs) / qMax<int64_t>(1, frameDurMs()))) * trackCount *
                    2;
                int packets = 0;
                while (!shouldInterrupt() && packets < kReverseChunkBudget) {
                    {
                        QMutexLocker locker(&m_mutex);
                        if (m_seekTargetMs >= 0) break;
                    }
                    int ret = av_read_frame(m_fmtCtx, pkt);
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
                keepFrom = P - (kTrailMs + kSlackMs);
                keepTo = P + (kLeadMs + kSlackMs);
            } else {
                keepFrom = P - (kLeadMs + kChunkMs + kSlackMs);
                keepTo = P + (kTrailMs + kSlackMs);
            }
            QMutexLocker bufferLocker(&m_bufferMutex);
            for (auto* track : m_decoderBank)
                track->buffer.trim(keepFrom, keepTo);
            if (m_outputCache) {
                const qint64 keepAudioFromSample = qMax<qint64>(0, keepFrom * qint64(48000) / 1000);
                m_outputCache->trimBefore(keepFrom, keepAudioFromSample);
                publishOutputCacheLocked();
            }
        }
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
                                               af.pcm.size(), af.ptsMs, P);
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
            armCutInternal(m_pendingRearmMs.load());
        }
        // Worker-private; never starves the primary tick (kPrerollPacketsPerTick
        // per pass). Stops once staging covers the armed window (m_stagingCovers),
        // at which point the cut is scheduled and fires on the output thread.
        if (m_cutArmed.load() && !m_stagingCovers.load()) fillStaging();

        // --- EOF / live-growth handling (§6.8) ---
        if (hitEof) {
            msleep(kEofSleepMs);
            int64_t sz = (m_fmtCtx->pb) ? avio_size(m_fmtCtx->pb) : -1;
            if (sz > m_sizeAtLastEof) {
                // Grown — un-latch and seek back to the reference newest so the
                // matroska demuxer's latched 'done' is cleared.
                if (m_fmtCtx->pb) {
                    m_fmtCtx->pb->eof_reached = 0;
                    m_fmtCtx->pb->error = 0;
                }
                avformat_flush(m_fmtCtx);
                int64_t rNewest = refNewestPts();
                AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
                int64_t anchorMs = qMax<int64_t>(0, rNewest);
                int64_t seekPts = av_rescale_q(anchorMs, {1, 1000}, vStream->time_base);
                int sret = av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
                m_sizeAtLastEof = sz;
                m_counters.eofTailSeek++;
                if (sret >= 0) {
                    // Dedup-before-decode: re-read tail clusters cost reads only.
                    // Drain a bounded number of packets, skipping already-buffered.
                    bool audioOn = playing && (speed > 0.99 && speed < 1.01);
                    const int kEofDrain = 4 * trackCount;
                    for (int i = 0; i < kEofDrain && !shouldInterrupt(); ++i) {
                        int ret = av_read_frame(m_fmtCtx, pkt);
                        if (ret < 0) break;
                        decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/ 1, trackCount,
                                             /*decimate*/ false, /*step*/ 1, audioOn,
                                             /*dedupTail*/ true);
                        av_packet_unref(pkt);
                    }
                    // Per-insert publish was removed; this path `continue`s past
                    // the run-loop trim, so publish the re-read tail frames once
                    // here so live-growth frames become visible promptly.
                    {
                        QMutexLocker bufferLocker(&m_bufferMutex);
                        publishOutputCacheLocked();
                    }
                }
                // else: seek failed — don't advance m_sizeAtLastEof handling,
                // retry next poll (sz already stored so we won't re-trigger
                // until further growth).
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
            bool windowFull = (dir >= 0) ? (nm >= 0 && nm >= P + kLeadMs)
                                         : (refOldestPts() >= 0 && refOldestPts() <= P - kLeadMs);
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
    m_hasPendingRearm.store(false);
    m_decoderFollowMs.store(-1);
}

void PlaybackWorker::deliverDueFrames(int64_t P, int dir) {
    struct PendingDeliver {
        FrameProvider* provider = nullptr;
        MediaVideoFrame frame;
    };

    QVector<PendingDeliver> pending;
    const bool outputGraphActive = m_outputRuntime != nullptr;
    const FrameRate rate = FrameRate::fromFraction(fps(), 1);
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
                    if (frame.frame.isValid()) {
                        placeholderWidth = frame.frame.width;
                        placeholderHeight = frame.frame.height;
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
                    frame.frame.feedIndex = track->feedIndex;
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
        }

        for (auto* track : m_decoderBank) {
            if (!track || !track->provider) continue;
            MediaVideoFrame f;
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

MediaVideoFrame PlaybackWorker::convertToMediaVideoFrame(AVFrame* frame, int feedIndex) {
    // Our recordings are always MPEG-2 all-intra YUV420P. Reject anything else
    // (a foreign MKV, 10-bit or 4:2:2 content) rather than copying it with the
    // wrong plane geometry and rendering garbage. Returns an invalid frame the
    // caller skips.
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width <= 0 || frame->height <= 0)
        return MediaVideoFrame();

    MediaVideoFrame out;
    out.feedIndex = feedIndex;
    out.width = frame->width;
    out.height = frame->height;
    out.format = MediaPixelFormat::Yuv420p;
    out.strideY = frame->width;
    out.strideU = (frame->width + 1) / 2;
    out.strideV = (frame->width + 1) / 2;
    const int chromaH = (frame->height + 1) / 2;
    // Allocate uninitialized: the per-line memcpy below overwrites every byte
    // up to copyW for all `height` lines, so a zero-fill is a dead store
    // (~3 MB memset per 1080p frame). Padding bytes (width..stride) are never
    // read by the renderer.
    out.planeY = QByteArray(out.strideY * frame->height, Qt::Uninitialized);
    out.planeU = QByteArray(out.strideU * chromaH, Qt::Uninitialized);
    out.planeV = QByteArray(out.strideV * chromaH, Qt::Uninitialized);

    QByteArray* dstPlanes[3] = {&out.planeY, &out.planeU, &out.planeV};
    const int dstStrides[3] = {out.strideY, out.strideU, out.strideV};
    for (int i = 0; i < 3; ++i) {
        uint8_t* src = frame->data[i];
        if (!src) return MediaVideoFrame();
        char* dst = dstPlanes[i]->data();
        int srcStride = frame->linesize[i];
        int dstStride = dstStrides[i];
        int height = (i == 0) ? frame->height : (frame->height + 1) / 2;
        int width = (i == 0) ? frame->width : (frame->width + 1) / 2;
        int copyW = qMin(width, qMin(qAbs(srcStride), dstStride));
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcLine =
                srcStride >= 0 ? (src + y * srcStride) : (src + (height - 1 - y) * -srcStride);
            memcpy(dst + y * dstStride, srcLine, size_t(copyW));
        }
    }

    return out;
}
