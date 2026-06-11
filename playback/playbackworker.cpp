#include "playback/playbackworker.h"
#include <QDebug>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <cstdio>
#include <cmath>
#include <cstdint>

PlaybackWorker::PlaybackWorker(const QList<FrameProvider*> &providers, PlaybackTransport *transport,
                               AudioPlayer *audioPlayer, QObject *parent)
    : QThread(parent)
{
    m_transport = transport;
    m_providers = providers;
    m_audioPlayer = audioPlayer;
}

PlaybackWorker::~PlaybackWorker() {
    stop();
    for (auto* track : m_decoderBank) {
        if (track->codecCtx) avcodec_free_context(&track->codecCtx);
        delete track;
    }
    for (auto* aTrack : m_audioDecoderBank) {
        if (aTrack->codecCtx) avcodec_free_context(&aTrack->codecCtx);
        delete aTrack;
    }
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
}

void PlaybackWorker::openFile(const QString &filePath) {
    QMutexLocker locker(&m_mutex);
    m_currentFilePath = filePath;
}

void PlaybackWorker::seekTo(int64_t timestampMs) {
    QMutexLocker locker(&m_mutex);
    m_seekTargetMs = qMax<int64_t>(0, timestampMs);
}

void PlaybackWorker::setActiveAudioView(int viewIndex) {
    int prev = m_activeAudioView.exchange(viewIndex, std::memory_order_relaxed);
    if (prev == viewIndex) return;   // no actual change

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
    fprintf(stderr, "SEC repos=%d reuse=%d revseek=%d eof=%d skip=%d apush=%d drop=%d P=%lld newest=%lld spd=%.2f\n",
            m_counters.reposition, m_counters.reuseSeek, m_counters.reverseChunkSeek,
            m_counters.eofTailSeek, m_counters.skipForward, m_counters.audioPushes,
            m_counters.framesDropped, (long long)P, (long long)newest, speed);
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
    return 1000 / fps();   // fps() >= 1 so no divide-by-zero
}

int PlaybackWorker::capFrames(int trackCount) const {
    // capFrames = clamp( ceil(windowMs / frameDurMs) + 4, 12,
    //                    max(12, kGlobalFrameBudget / max(1,trackCount)) )
    const int64_t windowMs = int64_t(kLeadMs) + kChunkMs + kTrailMs + 2 * int64_t(kSlackMs);
    const int64_t dur = qMax<int64_t>(1, frameDurMs());      // never divide by zero
    const int64_t ceilFrames = (windowMs + dur - 1) / dur;   // integer ceil
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
    if (maxNewest < 0) return -1;  // all tracks empty

    int64_t minNewest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n < 0) continue;                       // empty track
        if (n < maxNewest - kLeadMs) continue;     // stalled track — exclude
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
    if (maxNewest < 0) return -1;  // all tracks empty

    int64_t minOldest = -1;
    for (auto* track : m_decoderBank) {
        const int64_t n = track->buffer.newestPts();
        if (n < 0) continue;                       // empty track
        if (n < maxNewest - kLeadMs) continue;     // stalled track — exclude
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
    for (auto* track : m_decoderBank) track->lastDeliveredPtsMs = -1;
}

void PlaybackWorker::clearAllBuffers() {
    QMutexLocker bufferLocker(&m_bufferMutex);
    for (auto* track : m_decoderBank) {
        track->buffer.clear();
        track->decimateCounter = 0;
    }
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

// ---------------------------------------------------------------------------
// enqueueAudioFrame — format-guarded enqueue of active-view audio (spec §6.7).
// Mirrors the old pushAudioFrame format guard (S16 / 48k / stereo) but routes
// the PCM into the worker-side queue instead of pushing to AudioPlayer.
// ---------------------------------------------------------------------------
void PlaybackWorker::enqueueAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame,
                                      bool dedupTail) {
    if (audioFrame->format != AV_SAMPLE_FMT_S16
        || audioFrame->sample_rate != 48000
        || audioFrame->ch_layout.nb_channels != 2) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning() << "PlaybackWorker: unsupported audio frame format"
                       << audioFrame->format << audioFrame->sample_rate
                       << audioFrame->ch_layout.nb_channels;
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
    if (dedupTail && aTrack->lastEnqueuedPtsMs >= 0 && ptsMs <= aTrack->lastEnqueuedPtsMs)
        return;

    const int dataSize = audioFrame->nb_samples
                         * audioFrame->ch_layout.nb_channels
                         * int(sizeof(int16_t));
    m_audioQueue.enqueue(ptsMs, reinterpret_cast<const char*>(audioFrame->data[0]), dataSize);
    aTrack->lastEnqueuedPtsMs = ptsMs;
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
int64_t PlaybackWorker::decodePacketIntoBank(AVPacket* pkt, AVFrame* vf, AVFrame* af,
                                             int64_t P, int dir, int trackCount,
                                             bool decimate, int decimateStep,
                                             bool audioOn, bool dedupTail) {
    int64_t lastVideoPtsMs = INT64_MIN;
    const int cap = capFrames(trackCount);
    // Protect the active fill range in the travel direction (spec §6.6) so the
    // cap can never evict a frame the window still needs:
    //   forward: [P, P + kLeadMs]   reverse: [P - kLeadMs, P]
    const int64_t protectLo = (dir >= 0) ? P : (P - kLeadMs);
    const int64_t protectHi = (dir >= 0) ? (P + kLeadMs) : P;

    for (auto* track : m_decoderBank) {
        if (pkt->stream_index != track->streamIndex) continue;

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
                if (!keep) { av_frame_unref(vf); continue; }

                int64_t framePts = vf->pts;
                if (framePts == AV_NOPTS_VALUE) framePts = vf->best_effort_timestamp;
                AVRational tb = m_fmtCtx->streams[track->streamIndex]->time_base;
                int64_t framePtsMs;
                if (framePts != AV_NOPTS_VALUE) {
                    framePtsMs = av_rescale_q(framePts, tb, {1, 1000});
                } else {
                    // No PTS: synthesize from the last known, or fall back to P.
                    framePtsMs = (lastVideoPtsMs != INT64_MIN)
                                     ? lastVideoPtsMs + frameDurMs()
                                     : P;
                }

                // Dedup-before-decode after EOF un-latch: a re-read tail cluster
                // already in the buffer is skipped (read cost only, no dup).
                if (dedupTail) {
                    int64_t nv;
                    { QMutexLocker bufferLocker(&m_bufferMutex);
                      nv = track->buffer.newestPts(); }
                    if (nv >= 0 && framePtsMs <= nv) { av_frame_unref(vf); continue; }
                }

                QVideoFrame qFrame = convertToQVideoFrame(vf);
                {
                    QMutexLocker bufferLocker(&m_bufferMutex);
                    if (!track->buffer.insert(framePtsMs, qFrame, cap, protectLo, protectHi))
                        m_counters.framesDropped++;
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
void PlaybackWorker::repositionTo(int64_t target, int dir,
                                  AVPacket* pkt, AVFrame* vf, AVFrame* af) {
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
        return;
    }

    // --- Full reposition: clear everything, seek behind target, fill forward. ---
    clearAllBuffers();
    m_audioQueue.clear();
    for (auto* aTrack : m_audioDecoderBank) aTrack->lastEnqueuedPtsMs = -1;
    if (m_audioPlayer) m_audioPlayer->clear();

    const int64_t anchor = qMax<int64_t>(0, target - (dir < 0 ? kLeadMs : kTrailMs));

    AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
    int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
    av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
    // Intra-only: no avcodec_flush_buffers needed (spec §6.2); the seek itself
    // restarts the read cursor and the next sent packet decodes standalone.

    // Decode forward through target + frameDurMs, inserting all tracks. Audio is
    // re-primed by the normal forward release after the reposition, so we do NOT
    // enqueue here (audio queue stays empty until forward fill repopulates it).
    const int64_t fillTo = target + frameDurMs();
    int packets = 0;
    const int packetBudget = (capFrames(trackCount) + 4) * trackCount * 2;
    while (!shouldInterrupt()) {
        // A newer explicit seek supersedes this fill.
        { QMutexLocker locker(&m_mutex); if (m_seekTargetMs >= 0) break; }

        int ret = av_read_frame(m_fmtCtx, pkt);
        if (ret < 0) break;   // EOF/short file: deliver what we have

        // Reposition decodes forward from the anchor; protect the [target,
        // target+kLead] span (dir=+1) — the trail below target is also kept by
        // the anchor being kTrailMs/kLeadMs below it.
        decodePacketIntoBank(pkt, vf, af, target, /*dir*/1, trackCount,
                             /*decimate*/false, /*step*/1,
                             /*audioOn*/false, /*dedupTail*/false);
        av_packet_unref(pkt);

        if (++packets > packetBudget) break;          // safety bound
        if (newestPtsMin() >= fillTo) break;           // covered the target
    }

    resetDedup();
    deliverDueFrames(target, dir);
    m_counters.reposition++;
}

void PlaybackWorker::run() {
    qDebug() << "Opening file: "<<m_currentFilePath;

    if (m_currentFilePath.isEmpty()) return;

    // A stop() issued before/while we get here sets the interruption
    // flag, which (unlike m_running, re-set just below) survives — all
    // loops therefore gate on shouldInterrupt(), not m_running alone.
    m_running = true;

    auto clearDecoders = [this]() {
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (auto* track : m_decoderBank) {
            if (track->codecCtx) avcodec_free_context(&track->codecCtx);
            delete track;
        }
        m_decoderBank.clear();
        m_streamMap.clear();
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

        if (avformat_open_input(&newCtx, m_currentFilePath.toUtf8().constData(), nullptr, nullptr) < 0) {
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

                {
                    QMutexLocker bufferLocker(&m_bufferMutex);
                    m_decoderBank.append(track);
                    m_streamMap.insert(i, track);
                }

                providerIndex++;
                qDebug() << "Worker: Initialized Decoder for Stream" << i << "mapped to Provider" << (providerIndex-1);
            }
        }

        // Also detect audio streams (paired with video by order)
        int audioViewIdx = 0;
        for (unsigned int i = 0; i < m_fmtCtx->nb_streams; i++) {
            AVCodecParameters* codecParams = m_fmtCtx->streams[i]->codecpar;
            if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
                const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
                if (!codec) { audioViewIdx++; continue; }

                AVCodecContext* ctx = avcodec_alloc_context3(codec);
                if (!ctx) { audioViewIdx++; continue; }
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

                qDebug() << "Worker: Initialized Audio Decoder for Stream" << i
                         << "view" << audioViewIdx;
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
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();

    { QMutexLocker locker(&m_mutex); m_seekTargetMs = -1; }

    // Telemetry: emit a SEC line once per wall-second.
    int64_t lastTelemetryMs = 0;
    QElapsedTimer wallClock; wallClock.start();
    // EOF read-error backoff bound (non-EOF errors; §6.8).
    int readErrStreak = 0;
    const int kMaxReadErrStreak = 200;

    // ----------------------------------------------------------------------
    // THE WINDOWED SCHEDULER (spec §6).
    // ----------------------------------------------------------------------
    while (!shouldInterrupt()) {

        // --- Sample state (spec §6.1) ---
        int64_t P        = m_transport->currentPos();
        bool    playing  = m_transport->isPlaying();
        double  speed    = m_transport->speed();
        const double aspeed = qAbs(speed);
        int     dir      = playing ? (speed < 0 ? -1 : 1) : m_lastMoveDir;
        const int trackCount = qMax(1, int(m_decoderBank.size()));

        // --- Audio re-prime request from setActiveAudioView (UI thread) ---
        if (m_audioReprime.exchange(false, std::memory_order_relaxed)) {
            m_audioQueue.clear();
            if (m_audioPlayer) m_audioPlayer->clear();
        }

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
            dir = m_lastMoveDir;
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
                    QVideoFrame f; int64_t p = -1;
                    DecoderTrack* ref = m_decoderBank[0];
                    if (!ref->buffer.frameAt(P, f, p) || p != ref->lastDeliveredPtsMs)
                        needWork = true;
                } else {
                    needWork = true;
                }
            }
            if (!needWork) { msleep(10); continue; }
            // else: fall through to classify→deliver while paused.
        }

        // === CLASSIFY (spec §6.1, priority order) ===

        // (1) Explicit seek — coalesce to the latest target, clear it. → §6.2.
        int64_t seekTarget = -1;
        {
            QMutexLocker locker(&m_mutex);
            if (m_seekTargetMs >= 0) { seekTarget = m_seekTargetMs; m_seekTargetMs = -1; }
        }
        if (seekTarget >= 0) {
            // Anchor direction by the recorded move sign (seekTo sets it in T6).
            int seekDir = m_lastMoveDir;
            repositionTo(seekTarget, seekDir, pkt, frame, audioFrame);
            continue;
        }

        // (2) Backward jump: P fell below everything buffered. → §6.2, reverse.
        const int64_t oMin = oldestPtsMin();   // -1 if empty
        if (oMin >= 0 && P < oMin - kBackJumpSlackMs) {
            repositionTo(P, /*dir*/-1, pkt, frame, audioFrame);
            continue;
        }

        // (3) Forward lag / overrun (playing, dir=+1): decode/playhead is ahead
        //     of what's buffered. NEVER a reposition — skip-forward or tail-hold.
        const int64_t nMin = newestPtsMin();   // -1 if empty
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
                clearAllBuffers();
                m_audioQueue.clear();
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
        const int  decStep  = decimate ? int(std::ceil(aspeed)) : 1;
        bool hitEof = false;
        bool nonEofErr = false;
        int  packetsThisIter = 0;

        if (dir >= 0) {
            // --- Forward fill (§6.3): bounded to the window + one batch. ---
            const int kFillBatch = 4 * trackCount;
            int batch = 0;
            while (!shouldInterrupt() && batch < kFillBatch) {
                // Stop once the buffered min-newest reaches the lead edge.
                int64_t nm = newestPtsMin();
                if (nm >= 0 && nm >= P + kLeadMs) break;
                // Abort fill if a seek arrived.
                { QMutexLocker locker(&m_mutex); if (m_seekTargetMs >= 0) break; }

                int ret = av_read_frame(m_fmtCtx, pkt);
                if (ret == AVERROR_EOF) { hitEof = true; break; }
                if (ret < 0) { nonEofErr = true; break; }
                readErrStreak = 0;
                batch++;
                packetsThisIter++;

                // Audio enqueue only when at 1× forward single-view playing.
                bool audioOn = playing && dir == 1 && (speed > 0.99 && speed < 1.01);
                int64_t lastV = decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/1,
                                                     trackCount, decimate, decStep, audioOn,
                                                     /*dedupTail*/false);
                av_packet_unref(pkt);

                // Terminate when the just-read video packet crosses the slack edge.
                if (lastV != INT64_MIN && lastV > P + kLeadMs + kSlackMs) break;
            }
        } else {
            // --- Reverse fill (§6.4): fill-then-deliver one chunk atomically. ---
            const int64_t rOldest = refOldestPts();
            if (rOldest < 0 || (rOldest > P - kLeadMs && P > 0)) {
                // Record the avio position of the current oldest (file-position
                // terminator: well-defined under non-interleave skew).
                const int64_t stopPos = (m_fmtCtx->pb) ? avio_tell(m_fmtCtx->pb) : -1;

                AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
                int64_t anchor = qMax<int64_t>(0, P - kLeadMs - kChunkMs);
                int64_t seekPts = av_rescale_q(anchor, {1, 1000}, vStream->time_base);
                av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);
                m_counters.reverseChunkSeek++;

                const int kReverseChunkBudget =
                    int(std::ceil(double(kChunkMs) / qMax<int64_t>(1, frameDurMs())))
                    * trackCount * 2;
                int packets = 0;
                while (!shouldInterrupt() && packets < kReverseChunkBudget) {
                    { QMutexLocker locker(&m_mutex); if (m_seekTargetMs >= 0) break; }
                    int ret = av_read_frame(m_fmtCtx, pkt);
                    if (ret == AVERROR_EOF) { hitEof = true; break; }
                    if (ret < 0) { nonEofErr = true; break; }
                    readErrStreak = 0;
                    packets++;
                    packetsThisIter++;

                    // Decode forward WITHOUT delivering partial-chunk frames
                    // (audio muted in reverse). dir=-1 protects [P-kLead, P].
                    decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/-1,
                                         trackCount, decimate, decStep, /*audioOn*/false,
                                         /*dedupTail*/false);

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
                keepTo   = P + (kLeadMs + kSlackMs);
            } else {
                keepFrom = P - (kLeadMs + kChunkMs + kSlackMs);
                keepTo   = P + (kTrailMs + kSlackMs);
            }
            QMutexLocker bufferLocker(&m_bufferMutex);
            for (auto* track : m_decoderBank) track->buffer.trim(keepFrom, keepTo);
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
                    m_audioPlayer->pushSamples(
                        reinterpret_cast<const uint8_t*>(af.pcm.constData()),
                        af.pcm.size(), af.ptsMs, P);
                    m_counters.audioPushes++;
                }
            } else {
                // |speed|≠1 / reverse / multiview / muted: drop queued audio so
                // it can't be stale-re-released on return to 1× (§6.7).
                if (!m_audioQueue.isEmpty()) m_audioQueue.clear();
            }
        }

        // --- EOF / live-growth handling (§6.8) ---
        if (hitEof) {
            msleep(kEofSleepMs);
            int64_t sz = (m_fmtCtx->pb) ? avio_size(m_fmtCtx->pb) : -1;
            if (sz > m_sizeAtLastEof) {
                // Grown — un-latch and seek back to the reference newest so the
                // matroska demuxer's latched 'done' is cleared.
                if (m_fmtCtx->pb) { m_fmtCtx->pb->eof_reached = 0; m_fmtCtx->pb->error = 0; }
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
                        decodePacketIntoBank(pkt, frame, audioFrame, P, /*dir*/1,
                                             trackCount, /*decimate*/false, /*step*/1,
                                             audioOn, /*dedupTail*/true);
                        av_packet_unref(pkt);
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
            if (++readErrStreak > kMaxReadErrStreak) break;  // bounded: stop cleanly
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
}

bool PlaybackWorker::deliverBufferedFrameAtOrBefore(int64_t targetMs) {
    struct PendingDeliver {
        FrameProvider* provider = nullptr;
        QVideoFrame frame;
    };

    QVector<PendingDeliver> pending;
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (auto* track : m_decoderBank) {
            if (!track || !track->provider) continue;
            QVideoFrame f;
            int64_t p;
            if (track->buffer.frameAt(targetMs, f, p)) {
                track->lastDeliveredPtsMs = p;
                pending.append({track->provider, f});
            }
        }
    }

    if (pending.isEmpty()) return false;

    for (const auto &item : pending) {
        if (item.provider) item.provider->deliverFrame(item.frame);
    }
    return true;
}

void PlaybackWorker::deliverDueFrames(int64_t P, int dir) {
    struct PendingDeliver {
        FrameProvider* provider = nullptr;
        QVideoFrame frame;
    };

    QVector<PendingDeliver> pending;
    {
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (auto* track : m_decoderBank) {
            if (!track || !track->provider) continue;
            QVideoFrame f;
            int64_t p;
            if (track->buffer.frameAt(P, f, p)) {
                // Direction-aware dedup (spec §5): forbid out-of-order paints.
                //  - forward (+1): deliver iff pts moved up (or after a reset);
                //  - reverse (-1): deliver iff pts moved down (or after a reset).
                const int64_t last = track->lastDeliveredPtsMs;
                bool deliver;
                if (last < 0)            deliver = true;          // post-reset
                else if (dir >= 0)       deliver = (p > last);
                else                     deliver = (p < last);
                if (p == last) deliver = false;                  // already shown
                if (deliver) {
                    track->lastDeliveredPtsMs = p;
                    pending.append({track->provider, f});
                }
            }
        }
    }

    for (const auto &item : pending) {
        if (item.provider) item.provider->deliverFrame(item.frame);
    }
}

QVideoFrame PlaybackWorker::convertToQVideoFrame(AVFrame* frame) {
    // MPEG-2 All-Intra is typically YUV420P
    QVideoFrameFormat format(QSize(frame->width, frame->height), QVideoFrameFormat::Format_YUV420P);
    QVideoFrame qFrame(format);

    if (qFrame.map(QVideoFrame::WriteOnly)) {
        // Efficient copy of the YUV planes
        for (int i = 0; i < 3; ++i) {
            uint8_t* src = frame->data[i];
            uint8_t* dst = qFrame.bits(i);
            int srcStride = frame->linesize[i];
            int dstStride = qFrame.bytesPerLine(i);
            int height = (i == 0) ? frame->height : frame->height / 2;
            int width = (i == 0) ? frame->width : frame->width / 2;

            for (int y = 0; y < height; ++y) {
                memcpy(dst + y * dstStride, src + y * srcStride, width);
            }
        }
        qFrame.unmap();
    }

    // Qt's VideoSink will automatically handle the OpenGL texture upload
    // when this frame is delivered to the GPU-backed VideoOutput.
    return qFrame;
}
