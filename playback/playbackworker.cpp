#include "playback/playbackworker.h"
#include <QDebug>
#include <QMutexLocker>
#include <cstdio>

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

void PlaybackWorker::setFrameBufferMax(int maxFrames) {
    QMutexLocker bufferLocker(&m_bufferMutex);
    m_frameBufferMax = qMax(1, maxFrames);
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

void PlaybackWorker::repositionTo(int64_t target, int dir,
                                  AVPacket* pkt, AVFrame* vf, AVFrame* af) {
    // TODO Task 5: implement the windowed reposition (reuse fast-path + full
    // seek/decode-forward fill). Not called by the existing loop this task.
    (void)target; (void)dir; (void)pkt; (void)vf; (void)af;
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

    int64_t lastProcessedPtsMs = -1;
    { QMutexLocker locker(&m_mutex); m_seekTargetMs = -1; }

    while (!shouldInterrupt()) {

        // --- 2. GET MASTER TIME FROM TRANSPORT ---
        int64_t masterTimeMs = m_transport->currentPos();

        // --- PAUSE HANDLING ---
        // If playback is paused, block here until resumed — but still
        // react to pending seeks: paused scrubbing and frame stepping
        // must seek + burst-decode, or the frame buffer has nothing at
        // the target and the picture freezes.
        while (!shouldInterrupt() && !m_transport->isPlaying()) {
            {
                QMutexLocker locker(&m_mutex);
                if (m_seekTargetMs >= 0) break;
            }
            msleep(10);
            masterTimeMs = m_transport->currentPos();
        }

        // --- 3. CHECK FOR EXTERNAL SEEK (SCRUBBING) ---
        // If the master clock is far away from our last frame, trigger a hard seek
        m_mutex.lock();
        if (m_seekTargetMs >= 0 || (lastProcessedPtsMs >= 0 && qAbs(masterTimeMs - lastProcessedPtsMs) > 500)) {
            int64_t target = (m_seekTargetMs >= 0) ? m_seekTargetMs : masterTimeMs;

            // Map MS to stream timebase (using the first video track as reference)
            if (m_decoderBank.isEmpty()) {
                m_mutex.unlock();
                msleep(10);
                continue;
            }
            AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
            int64_t seekPts = av_rescale_q(target, {1, 1000}, vStream->time_base);

            av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);

            // Flush all decoders in the bank
            for(auto* track : m_decoderBank) {
                if(track->codecCtx) avcodec_flush_buffers(track->codecCtx);
            }
            for (auto* aTrack : m_audioDecoderBank) {
                if (aTrack->codecCtx) avcodec_flush_buffers(aTrack->codecCtx);
            }
            if (m_audioPlayer) m_audioPlayer->clear();

            // Clear stale frames from the buffer to prevent visual artifacts
            {
                QMutexLocker bufferLocker(&m_bufferMutex);
                for (auto* track : m_decoderBank) {
                    track->buffer.clear();
                }
            }

            lastProcessedPtsMs = target;
            m_seekTargetMs = -1;
            m_mutex.unlock();

            // Burst-decode to pre-fill the buffer so backward stepping works immediately after seek
            {
                int packetCount = 0;
                const int bufMax = m_frameBufferMax;
                const int trackCount = qMax(1, int(m_decoderBank.size()));
                // Budget per track: a global frame count gave each track
                // only bufMax/N frames of back-step room with N tracks.
                const int packetMax = bufMax * 4 * trackCount;
                QHash<DecoderTrack*, int> burstDecoded;
                auto allTracksFull = [&]() {
                    for (auto* t : m_decoderBank)
                        if (burstDecoded.value(t, 0) < bufMax) return false;
                    return true;
                };
                while (!shouldInterrupt() && !allTracksFull() && packetCount < packetMax) {
                    // Abort burst if a new seek was requested
                    {
                        QMutexLocker locker(&m_mutex);
                        if (m_seekTargetMs >= 0) break;
                    }

                    int ret = av_read_frame(m_fmtCtx, pkt);
                    if (ret < 0) break;
                    packetCount++;

                    for (auto* track : m_decoderBank) {
                        if (pkt->stream_index == track->streamIndex) {
                            if (avcodec_send_packet(track->codecCtx, pkt) == 0) {
                                while (avcodec_receive_frame(track->codecCtx, frame) == 0) {
                                    int64_t framePts = frame->pts;
                                    if (framePts == AV_NOPTS_VALUE) framePts = frame->best_effort_timestamp;
                                    AVRational tb = m_fmtCtx->streams[track->streamIndex]->time_base;
                                    int64_t framePtsMs = lastProcessedPtsMs;
                                    if (framePts != AV_NOPTS_VALUE) {
                                        framePtsMs = av_rescale_q(framePts, tb, {1, 1000});
                                        lastProcessedPtsMs = framePtsMs;
                                    } else if (lastProcessedPtsMs >= 0) {
                                        framePtsMs = lastProcessedPtsMs + 33;
                                        lastProcessedPtsMs = framePtsMs;
                                    }

                                    QVideoFrame qFrame = convertToQVideoFrame(frame);
                                    {
                                        QMutexLocker bufferLocker(&m_bufferMutex);
                                        track->buffer.insert(framePtsMs, qFrame, m_frameBufferMax,
                                                             framePtsMs, framePtsMs);
                                    }
                                    burstDecoded[track]++;
                                }
                            }
                            break;
                        }
                    }

                    // Also keep audio decoders warm during burst (discard output)
                    for (auto* aTrack : m_audioDecoderBank) {
                        if (pkt->stream_index == aTrack->streamIndex) {
                            if (avcodec_send_packet(aTrack->codecCtx, pkt) == 0) {
                                while (avcodec_receive_frame(aTrack->codecCtx, audioFrame) == 0) {
                                    // If this is the active audio view, push to the ring buffer
                                    int activeView = m_activeAudioView.load(std::memory_order_relaxed);
                                    if (activeView == aTrack->viewIndex && m_audioPlayer) {
                                        double spd = m_transport->speed();
                                        bool normalSpeed = (spd > 0.99 && spd < 1.01);
                                        if (normalSpeed && m_transport->isPlaying()) {
                                            pushAudioFrame(aTrack, audioFrame);
                                        }
                                    }
                                    av_frame_unref(audioFrame);
                                }
                            }
                            break;
                        }
                    }

                    av_packet_unref(pkt);
                }

                // Deliver the best buffered frame for the seek target
                deliverBufferedFrameAtOrBefore(target);
            }

            // Reset to the seek target so the pace control doesn't see
            // the burst-decoded PTS (which is ~1 s ahead) and gate audio.
            lastProcessedPtsMs = target;

            continue;
        }

        m_mutex.unlock();

        // --- 4. & 5. READ & DECODE ---
        // Always read packets — video and audio are interleaved in the MKV,
        // so blocking the read loop to pace video also starves audio.
        // Video frames are buffered; the existing buffer-cap prevents runaway
        // memory.  Pace control (sleep) happens AFTER the read, not before it.
        int readResult = av_read_frame(m_fmtCtx, pkt);

        if (readResult >= 0) {
            for (auto* track : m_decoderBank) {
                if (pkt->stream_index == track->streamIndex) {
                    if (avcodec_send_packet(track->codecCtx, pkt) == 0) {
                        while (avcodec_receive_frame(track->codecCtx, frame) == 0) {

                            // Convert frame PTS to Milliseconds (with fallback)
                            int64_t framePts = frame->pts;
                            if (framePts == AV_NOPTS_VALUE) framePts = frame->best_effort_timestamp;
                            AVRational tb = m_fmtCtx->streams[track->streamIndex]->time_base;
                            int64_t framePtsMs = lastProcessedPtsMs;
                            if (framePts != AV_NOPTS_VALUE) {
                                framePtsMs = av_rescale_q(framePts, tb, {1, 1000});
                                lastProcessedPtsMs = framePtsMs;
                            } else if (lastProcessedPtsMs >= 0) {
                                framePtsMs = lastProcessedPtsMs + 33; // fallback 30fps
                                lastProcessedPtsMs = framePtsMs;
                            } else {
                                framePtsMs = masterTimeMs;
                                lastProcessedPtsMs = framePtsMs;
                            }
                            QVideoFrame qFrame = convertToQVideoFrame(frame);
                            {
                                QMutexLocker bufferLocker(&m_bufferMutex);
                                track->buffer.insert(framePtsMs, qFrame, m_frameBufferMax,
                                                     framePtsMs, framePtsMs);
                            }
                            // NOTE: no immediate delivery here — frames are
                            // released against the master clock below, so the
                            // picture is not shown up to 100 ms early (the
                            // decode lead allowed by the pace gate).
                        }
                    }
                    break;
                }
            }

            // --- AUDIO DECODE: keep ALL decoders running, route active view ---
            // Decoding every audio stream continuously (PCM = near-zero cost)
            // ensures that switching views is instant — no cold-start gap.
            for (auto* aTrack : m_audioDecoderBank) {
                if (pkt->stream_index != aTrack->streamIndex) continue;

                if (avcodec_send_packet(aTrack->codecCtx, pkt) == 0) {
                    while (avcodec_receive_frame(aTrack->codecCtx, audioFrame) == 0) {
                        // Only push to speaker if this is the selected view
                        int activeView = m_activeAudioView.load(std::memory_order_relaxed);
                        if (activeView == aTrack->viewIndex && m_audioPlayer) {
                            double spd = m_transport->speed();
                            bool normalSpeed = (spd > 0.99 && spd < 1.01);
                            if (normalSpeed && m_transport->isPlaying()) {
                                pushAudioFrame(aTrack, audioFrame);
                            }
                        }
                        av_frame_unref(audioFrame);
                    }
                }
                break;  // packet matched this track, no need to check others
            }

            av_packet_unref(pkt);
        }
        else if (readResult == AVERROR_EOF) {
            // Live-file handling: Reset EOF flags and wait for Muxer
            if (m_fmtCtx->pb) {
                m_fmtCtx->pb->eof_reached = 0;
                m_fmtCtx->pb->error = 0;
            }
            msleep(10);
            avformat_flush(m_fmtCtx);
        }

        // --- DELIVER DUE FRAMES ---
        // Release the newest buffered frame whose PTS is due on the
        // master clock.  Decode runs ahead (see pace control), so
        // delivering at decode time would show frames early and jittery.
        deliverDueFrames(m_transport->currentPos());

        // --- PACE CONTROL ---
        // Throttle when we've decoded well ahead of the master clock.
        // This MUST happen after reading/decoding so that audio packets
        // (interleaved with video in the MKV) are never starved.
        // The 100 ms margin keeps the audio ring buffer healthy — without
        // it, the pace gate starves audio delivery and causes underruns
        // (audible as a rattle after seek).
        if (lastProcessedPtsMs > masterTimeMs + 100) {
            msleep(5);
        }
    }

    // --- 6. CLEANUP ---
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&audioFrame);
    clearDecoders();
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
}

void PlaybackWorker::pushAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame) {
    // The app's recordings carry PCM S16LE 48 kHz stereo; anything else
    // would need resampling before it can go to the audio device.
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
    const int dataSize = audioFrame->nb_samples
                         * audioFrame->ch_layout.nb_channels
                         * int(sizeof(int16_t));
    m_audioPlayer->pushSamples(audioFrame->data[0], dataSize,
                               ptsMs, m_transport->currentPos());
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

void PlaybackWorker::deliverDueFrames(int64_t masterTimeMs) {
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
            if (track->buffer.frameAt(masterTimeMs, f, p)) {
                if (p != track->lastDeliveredPtsMs) {
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
