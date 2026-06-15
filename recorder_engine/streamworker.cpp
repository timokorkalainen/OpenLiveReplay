#include "streamworker.h"
#include <QDebug>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>

StreamWorker::StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock *clock,
                           int targetWidth, int targetHeight, int targetFps, QObject* parent)
    : QThread(parent), m_url(url), m_sourceIndex(sourceIndex), m_viewTrack(-1), m_muxer(muxer), m_sharedClock(clock) {
    m_restartCapture = 0;
    m_internalFrameCount = 0;
    m_monotonic.start();
    if (targetWidth > 0) m_targetWidth = targetWidth;
    if (targetHeight > 0) m_targetHeight = targetHeight;
    if (targetFps > 0) m_targetFps = targetFps;
}

StreamWorker::~StreamWorker() { stop(); wait(); }

void StreamWorker::setConnected(bool c) {
    // exchange first so the emit fires exactly once per real transition,
    // even if two capture-thread call sites race the same value.
    const bool prev = m_connected.exchange(c, std::memory_order_relaxed);
    if (prev != c) {
        emit connectionChanged(m_sourceIndex, c);
    }
}

void debugTimestamp(const QString &prefix, int trackIndex) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    qDebug() << "[" << timeStr << "] [Track" << trackIndex << "]" << prefix;
}

void StreamWorker::stop() {
    m_restartCapture = 1;
    m_captureRunning = false;
    this->quit();
}

int StreamWorker::ffmpegInterruptCallback(void* opaque) {
    StreamWorker* worker = static_cast<StreamWorker*>(opaque);
    return worker ? worker->shouldInterrupt() : 0;
}

bool StreamWorker::shouldInterrupt() const {
    if (!m_captureRunning || m_restartCapture) return true;
    const int64_t lastPkt = m_lastPacketAtMs.load(std::memory_order_relaxed);
    if (m_connected && lastPkt >= 0
        && m_monotonic.elapsed() - lastPkt > m_stallTimeoutMs) return true;
    return false;
}

void StreamWorker::run() {
    // 1. Setup the persistent encoder context
    if (!setupEncoder(&m_persistentEncCtx)) return;

    // 2. Enter the event loop. The thread stays alive, waiting for
    // signals (masterPulse) or concurrent tasks (captureLoop).
    exec();

    // exec() has returned, so no further queued pulse can run on this
    // thread.  Re-assert shutdown before joining the capture thread: a
    // pulse delivered between stop() and quit() taking effect could have
    // started captureLoop after stop() cleared the flags, which would
    // otherwise leave join() stuck forever.
    m_restartCapture = 1;
    m_captureRunning = false;
    // The capture thread is started and joined on this (the worker) thread
    // only, so there is no cross-thread race on m_captureThread itself.
    if (m_captureThread.joinable()) m_captureThread.join();

    // Cleanup when exec() returns (on stop)
    avcodec_free_context(&m_persistentEncCtx);
    av_frame_free(&m_latestFrame);

    // Free the scaler cached across the capture lifetime (Bug 2: this was
    // never freed, leaking one SwsContext per record/stop cycle per source).
    // Safe here: the capture thread has been joined, so nothing touches
    // m_swsCtx after this point.
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    while(!m_frameQueue.isEmpty()){
        auto qf = m_frameQueue.dequeue();
        av_frame_free(&qf.frame);
    }
}

void StreamWorker::onMasterPulse(int64_t frameIndex, int64_t streamTimeMs) {
    m_internalFrameCount = frameIndex;

    // Snapshot the trim once per pulse so video and audio of this tick use the
    // SAME offset even if the UI thread changes it mid-pulse (keeps A/V locked).
    const int64_t trimMs = m_trimOffsetMs.load(std::memory_order_relaxed);

    // Publish this tick's jitter-pull gate for the capture thread's
    // queue pre-drain (see captureLoop).
    m_lastTickTargetMs.store(
        qMax<int64_t>(0, (frameIndex * 1000) / m_targetFps - kJitterBufferMs - trimMs),
        std::memory_order_relaxed);

    if (!m_persistentEncCtx) return;

    // Stall detection: if connected but no frames for too long, signal restart.
    const int64_t lastEnq = m_lastFrameEnqueueAtMs.load(std::memory_order_relaxed);
    if (m_captureRunning && m_connected && lastEnq >= 0
        && m_monotonic.elapsed() - lastEnq > m_stallTimeoutMs) {
        qDebug() << "Source" << m_sourceIndex << "No frames queued. Forcing restart...";
        m_restartCapture = 1;
    }

    // Start the dedicated capture thread exactly once, on the first pulse.
    // captureLoop() loops internally on reconnect/URL-change (m_restartCapture)
    // and only returns when m_captureRunning goes false, so it never needs
    // re-launching.  We start it on its own std::thread (not the shared
    // global pool) so N infinite captureLoops can't saturate that pool.
    // Both start (here) and join (in run() post-exec) happen on the worker
    // thread, so m_captureThread is never touched cross-thread.
    if (!m_captureThread.joinable()) {
        m_restartCapture = 0;
        m_captureRunning = true;
        m_captureThread = std::thread([this]() { this->captureLoop(); });
    }

    processEncoderTick(m_persistentEncCtx, streamTimeMs, trimMs);
}

void StreamWorker::processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs,
                                      int64_t trimMs) {
    AVPacket* outPkt = av_packet_alloc();
    bool havePacket = false;
    int track = -1;
    const int64_t currentRecordingTimeMs = (m_internalFrameCount * 1000) / m_targetFps;

    AVFrame* pulled = nullptr;
    const bool paintBlue = m_paintBlue.fetchAndStoreRelaxed(0) != 0;

    // The mutex only guards m_frameQueue (shared with the capture
    // thread).  m_latestFrame and the encoder are tick-thread-only,
    // so painting/encoding happens outside the lock.
    {
        QMutexLocker locker(&m_frameMutex);

        if (paintBlue) {
            while (!m_frameQueue.isEmpty()) {
                auto qf = m_frameQueue.dequeue();
                av_frame_free(&qf.frame);
            }
        }

        // ALWAYS do jitter pull to keep m_latestFrame fresh, even when
        // not assigned to a view.  This ensures frames are ready the
        // instant this source gets mapped to a view.
        int64_t targetTimeMs = currentRecordingTimeMs - kJitterBufferMs - trimMs;
        if (targetTimeMs < 0) targetTimeMs = 0;

        while (!m_frameQueue.isEmpty() && m_frameQueue.head().sourcePts <= targetTimeMs) {
            QueuedFrame top = m_frameQueue.dequeue();
            if (pulled) av_frame_free(&pulled);
            pulled = top.frame;
        }
    }

    if (paintBlue && m_latestFrame && m_latestFrame->data[0]) {
        memset(m_latestFrame->data[0], 128, m_latestFrame->linesize[0] * m_latestFrame->height);
        memset(m_latestFrame->data[1], 240,
               m_latestFrame->linesize[1] *
                   (m_latestFrame->height / 2)); // Cb: 240 = legal max chroma (255 is out-of-range)
        memset(m_latestFrame->data[2], 107, m_latestFrame->linesize[2] * (m_latestFrame->height / 2));
    }
    if (pulled) {
        av_frame_unref(m_latestFrame);
        av_frame_move_ref(m_latestFrame, pulled);
        av_frame_free(&pulled);
    }

    // Read the current view-track assignment (atomic, set by UIManager).
    // -1 = this source is not assigned to any view, skip encoding.
    track = m_viewTrack.load(std::memory_order_relaxed);

    if (track >= 0 && m_latestFrame && m_latestFrame->data[0]) {
        // Set PTS on the FRAME, not the packet (avcodec_receive_packet
        // overwrites the packet entirely).
        m_latestFrame->pts = m_internalFrameCount;

        if (avcodec_send_frame(encCtx, m_latestFrame) == 0) {
            if (avcodec_receive_packet(encCtx, outPkt) == 0) {
                outPkt->stream_index = track;
                outPkt->duration = 1;
                AVStream* st = m_muxer->getStream(track);
                if (st) {
                    av_packet_rescale_ts(outPkt, encCtx->time_base, st->time_base);
                    havePacket = true;
                }
            }
        }
    }

    if (havePacket) {
        m_muxer->writePacket(outPkt);

        // Write the per-frame source metadata to the paired subtitle track
        QByteArray metaJson;
        { QMutexLocker locker(&m_metadataMutex); metaJson = m_sourceMetadataJson; }
        if (!metaJson.isEmpty()) {
            m_muxer->writeMetadataPacket(track, streamTimeMs, metaJson);
        }
    }
    av_packet_free(&outPkt);

    // Write this tick's worth of audio for the assigned view track
    // (sample-accurate cursor, silence-filled where capture had nothing).
    writeAudioForTick(currentRecordingTimeMs, track, trimMs);
}

void StreamWorker::captureLoop() {
    while (m_captureRunning) {
        // If a restart was requested (e.g. changeSource), acknowledge it
        // and loop back to re-read the URL instead of exiting.
        m_restartCapture = 0;

        AVFormatContext* inCtx = nullptr;
        AVCodecContext* decCtx = nullptr;

        QString currentUrl;
        { QMutexLocker locker(&m_urlMutex); currentUrl = m_url; }

        // If URL is empty, don't attempt to connect. Just idle until
        // a new URL is set via changeSource() which sets m_restartCapture.
        if (currentUrl.trimmed().isEmpty()) {
            setConnected(false);
            while (m_captureRunning && !m_restartCapture) {
                QThread::msleep(100);
            }
            continue;
        }

        qDebug() << "Source" << m_sourceIndex << "Attempting connection to:" << currentUrl;
        setConnected(false);

        int videoStreamIdx = -1;
        if (!setupDecoder(&inCtx, &decCtx, currentUrl, &videoStreamIdx)) {
            qDebug() << "Source" << m_sourceIndex << "Connect failed. Retrying in" << (m_connectBackoffMs / 1000.0) << "s...";
            const int steps = qMax(1, m_connectBackoffMs / 100);
            for (int i = 0; i < steps && m_captureRunning && !m_restartCapture; ++i) {
                QThread::msleep(100);
            }
            if (!m_restartCapture) {
                m_connectBackoffMs = qMin(10000, m_connectBackoffMs * 2);
            }
            continue;
        }
        setConnected(true);
        m_connectBackoffMs = 1000;
        // A real source connected: stragglers from any previously-cleared
        // source are gone, so resume enqueuing frames for this new URL.
        m_suppressEnqueue.store(false, std::memory_order_relaxed);

        if (!m_sharedClock) {
            qDebug() << "Source" << m_sourceIndex << "No shared clock. Restarting...";
            m_restartCapture = 1;
            m_captureRunning = false;
            break;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* rawFrame = av_frame_alloc();
        AVFrame* audioFrame = av_frame_alloc();
        if (!pkt || !rawFrame || !audioFrame) {
            qDebug() << "Source" << m_sourceIndex << "Failed to allocate packet/frame.";
            if (pkt) av_packet_free(&pkt);
            if (rawFrame) av_frame_free(&rawFrame);
            if (audioFrame) av_frame_free(&audioFrame);
            avcodec_free_context(&decCtx);
            avformat_close_input(&inCtx);
            continue;
        }

        // ── Audio decoder & resampler setup ─────────────────────────
        int audioStreamIdx = -1;
        AVCodecContext* audioDecCtx = nullptr;
        SwrContext* swrCtx = nullptr;

        // Fix 4: the input audio config can change mid-stream (5.1->stereo at
        // ad breaks, stereo->mono, sample-rate change).  swrCtx is configured
        // once, so a stale resampler then reads a non-existent extended_data
        // plane on a layout shrink (segfault) or resamples at the wrong ratio
        // on a rate change (wrong pitch + FIFO drift).  We remember the input
        // params swrCtx was built with and, before each frame, rebuild swr if
        // the live frame's params differ.  The OUTPUT stays invariant (48 kHz
        // stereo S16).
        int swrInRate = 0;
        AVSampleFormat swrInFmt = AV_SAMPLE_FMT_NONE;
        AVChannelLayout swrInLayout;
        memset(&swrInLayout, 0, sizeof(swrInLayout));

        // Build (or rebuild) swrCtx for the given input params.  Frees any
        // existing context first, records the configured input params on
        // success, and leaves swrCtx == nullptr on failure (caller skips the
        // frame rather than crashing).  Output is always 48 kHz stereo S16.
        auto configureSwr = [&](int inRate, AVSampleFormat inFmt,
                                const AVChannelLayout* inLayout) -> bool {
            if (swrCtx) swr_free(&swrCtx);
            av_channel_layout_uninit(&swrInLayout);

            // Always output stereo.  swresample's standard rematrixing handles
            // the channel conversion: mono is duplicated into L/R,
            // multichannel is downmixed.  (A manual swr_set_channel_mapping
            // with 2 entries for a 1-channel input layout corrupts the
            // resampler state and crashes with SIGBUS inside swr_convert.)
            AVChannelLayout outLayout;
            av_channel_layout_default(&outLayout, 2);

            int ret = swr_alloc_set_opts2(&swrCtx, &outLayout, AV_SAMPLE_FMT_S16, 48000, inLayout,
                                          inFmt, inRate, 0, nullptr);
            av_channel_layout_uninit(&outLayout);
            if (ret < 0 || swr_init(swrCtx) < 0) {
                if (swrCtx) swr_free(&swrCtx);
                swrCtx = nullptr;
                qDebug() << "Source" << m_sourceIndex << "Failed to init audio resampler";
                return false;
            }
            swrInRate = inRate;
            swrInFmt = inFmt;
            av_channel_layout_copy(&swrInLayout, inLayout);
            qDebug() << "Source" << m_sourceIndex << "Audio: rate=" << inRate
                     << "ch=" << inLayout->nb_channels << "→ 48000 Hz stereo";
            return true;
        };
        {
            const AVCodec* audioDecoder = nullptr;
            int foundAudioIdx = av_find_best_stream(inCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
            if (foundAudioIdx >= 0 && audioDecoder) {
                audioDecCtx = avcodec_alloc_context3(audioDecoder);
                avcodec_parameters_to_context(audioDecCtx, inCtx->streams[foundAudioIdx]->codecpar);
                if (avcodec_open2(audioDecCtx, audioDecoder, nullptr) >= 0) {
                    audioStreamIdx = foundAudioIdx;
                    configureSwr(audioDecCtx->sample_rate, audioDecCtx->sample_fmt,
                                 &audioDecCtx->ch_layout);
                } else {
                    avcodec_free_context(&audioDecCtx);
                    audioDecCtx = nullptr;
                }
            }
        }
        int64_t anchorStreamTimeMs = -1;
        // Fix 3: use AV_NOPTS_VALUE (INT64_MIN) as the unset sentinel, not
        // -1.  A stream whose first video packet legitimately has dts == -1
        // (negative start offset / encoder priming) would keep matching a
        // -1 sentinel and re-anchor on every packet until a non--1 DTS
        // arrives, glitching session start.  -1 is a valid DTS; INT64_MIN
        // is not, and it matches the missing-timestamp checks used elsewhere.
        int64_t firstPacketDts = AV_NOPTS_VALUE;
        // Fix 2: DTS of the previous video packet (in this stream's own
        // time_base), used to detect mid-stream discontinuities.
        int64_t prevPktDts = AV_NOPTS_VALUE;

        // Audio gets its OWN anchor + discontinuity detection, independent of
        // video.  Sources where audio and video carry independent timestamp
        // domains (RTMP), or where only one of the two has a discontinuity,
        // must not let a video-side re-anchor shift the audio mapping (and
        // vice versa).  These mirror the video anchor locals but live entirely
        // on the audio stream's own presentation timestamps / time_base.  On a
        // clean MPEG-TS stream where A/V share the clock, both anchors sample
        // m_sharedClock->elapsedMs() at ~the same first-packet moment, so the
        // alignment is identical to the old containerStartMs-based mapping.
        int64_t firstAudioDts = AV_NOPTS_VALUE;
        int64_t audioAnchorStreamTimeMs = -1;
        int64_t prevAudioDts = AV_NOPTS_VALUE;

        m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
        m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);

        while (m_captureRunning && !m_restartCapture) {
            int readResult = av_read_frame(inCtx, pkt);
            if (readResult >= 0) {
                m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
                if (pkt->stream_index == videoStreamIdx) {

                    int64_t pktDts = pkt->dts;
                    if (pktDts == AV_NOPTS_VALUE) pktDts = pkt->pts;
                    if (pktDts == AV_NOPTS_VALUE) {
                        av_packet_unref(pkt);
                        continue;
                    }

                    const AVRational videoTb = inCtx->streams[videoStreamIdx]->time_base;

                    // 1. Establish (or re-establish) the Anchor.
                    //
                    // Fix 2 — discontinuity / wrap re-anchor.  The anchor is
                    // otherwise set ONCE per connection.  A mid-stream MPEG-TS
                    // discontinuity, upstream encoder restart, RTMP timestamp
                    // reset, or 33-bit PCR wrap makes pktDts jump by a large
                    // amount.  A forward jump would push sourcePts far into the
                    // future (never passes the tick gate -> video frozen on the
                    // last pre-jump frame); a backward jump (wrap) would map
                    // audio hours behind the cursor -> permanent silence.  The
                    // stall detector never fires because packets keep arriving.
                    //
                    // We detect a jump by comparing this packet's DTS to the
                    // previous one, in real-time milliseconds.  Normal
                    // inter-packet deltas are a frame duration (tens of ms); a
                    // real discontinuity is seconds.  Threshold: a forward jump
                    // > 3000 ms, OR any backward step beyond a small tolerance
                    // (-200 ms; allows benign B-frame/PTS reordering jitter but
                    // catches a wrap).  On a jump we re-anchor exactly like the
                    // initial anchor: firstPacketDts from THIS packet,
                    // anchorStreamTimeMs = now.  The audio path keeps its OWN
                    // anchor (firstAudioDts / audioAnchorStreamTimeMs), so a
                    // video-only discontinuity here never corrupts audio.
                    bool needAnchor = (firstPacketDts == AV_NOPTS_VALUE);
                    if (!needAnchor && prevPktDts != AV_NOPTS_VALUE) {
                        const int64_t deltaMs =
                            av_rescale_q(pktDts - prevPktDts, videoTb, {1, 1000});
                        constexpr int64_t kForwardJumpMs = 3000;
                        constexpr int64_t kBackwardTolMs = -200;
                        if (deltaMs > kForwardJumpMs || deltaMs < kBackwardTolMs) {
                            qDebug() << "Source" << m_sourceIndex << "DTS discontinuity ("
                                     << deltaMs << "ms jump). Re-anchoring.";
                            needAnchor = true;
                        }
                    }
                    if (needAnchor) {
                        firstPacketDts = pktDts;
                        // Where are we in the global recording right now?
                        anchorStreamTimeMs = m_sharedClock->elapsedMs();
                    }
                    prevPktDts = pktDts;

                    if (avcodec_send_packet(decCtx, pkt) >= 0) {
                        while (avcodec_receive_frame(decCtx, rawFrame) >= 0) {

                            // Bug 4: a restart/blue-paint is pending (source
                            // was cleared to an empty URL).  Drop frames from
                            // the old source so a late straggler can't
                            // overwrite the painted blue frame.  Still unref
                            // rawFrame so the decoder can reuse the buffer.
                            if (m_suppressEnqueue.load(std::memory_order_relaxed)) {
                                av_frame_unref(rawFrame);
                                continue;
                            }

                            // 2. Prepare the container for the scaled frame
                            QueuedFrame qf;
                            qf.frame = av_frame_alloc();
                            // Bug 3: av_frame_alloc can fail under memory
                            // pressure.  Skip this frame rather than deref NULL.
                            if (!qf.frame) {
                                av_frame_unref(rawFrame);
                                continue;
                            }
                            qf.frame->format = AV_PIX_FMT_YUV420P;
                            qf.frame->width = m_targetWidth;
                            qf.frame->height = m_targetHeight;
                            // Bug 3: a failed buffer alloc leaves a data-less
                            // frame; never enqueue that.  Free and skip.
                            if (av_frame_get_buffer(qf.frame, 0) < 0) {
                                av_frame_free(&qf.frame);
                                av_frame_unref(rawFrame);
                                continue;
                            }

                            // 3. Scale
                            m_swsCtx = sws_getCachedContext(m_swsCtx,
                                                            rawFrame->width, rawFrame->height, (AVPixelFormat)rawFrame->format,
                                                            m_targetWidth, m_targetHeight, AV_PIX_FMT_YUV420P,
                                                            SWS_BICUBIC, nullptr, nullptr, nullptr);
                            // Bug 3: getCachedContext returns NULL on an
                            // unsupported pixfmt/params.  Don't sws_scale a
                            // NULL context — free the frame and skip.
                            if (!m_swsCtx) {
                                av_frame_free(&qf.frame);
                                av_frame_unref(rawFrame);
                                continue;
                            }

                            sws_scale(m_swsCtx, rawFrame->data, rawFrame->linesize, 0, rawFrame->height,
                                      qf.frame->data, qf.frame->linesize);

                            // 2. Calculate the RELATIVE offset of THIS decoded
                            // frame in its own stream.
                            //
                            // Bug 3: every frame drained from this one
                            // avcodec_receive_frame loop must be stamped from
                            // ITS OWN display timestamp, not the shared outer
                            // pktDts.  On B-frame sources (common H.264) one
                            // packet yields several frames in display order;
                            // stamping them all from pktDts gives identical
                            // sourcePts, and the pre-drain keeps only one of
                            // each duplicate -> dropped frames / choppy video.
                            // best_effort_timestamp is the frame's display PTS
                            // in the video stream's time_base (fall back to
                            // pktDts when absent).  Discontinuity detection and
                            // the anchor stay on pktDts above (packet-level is
                            // fine for that); only this per-frame STAMP changes.
                            int64_t frameTs = rawFrame->best_effort_timestamp;
                            if (frameTs == AV_NOPTS_VALUE) frameTs = pktDts;
                            int64_t relativeMs =
                                av_rescale_q(frameTs - firstPacketDts, videoTb, {1, 1000});

                            // 3. Map it to the Global Timeline
                            // If a burst of 10 frames arrives, relativeMs increases by 33ms each,
                            // spacing them out perfectly in our queue.
                            qf.sourcePts = anchorStreamTimeMs + relativeMs;

                            QMutexLocker locker(&m_frameMutex);
                            m_frameQueue.enqueue(qf);

                            m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);

                            // Pre-drain frames the next tick would discard
                            // anyway: the tick keeps only the newest frame
                            // at-or-before its gate, so the head is garbage
                            // as soon as a SECOND frame is inside the gate.
                            // Never drops the newest gated frame — a lagging
                            // source shows late-but-updating video instead
                            // of freezing.
                            const int64_t tickGateMs =
                                m_lastTickTargetMs.load(std::memory_order_relaxed);
                            while (tickGateMs >= 0 && m_frameQueue.size() >= 2
                                   && m_frameQueue.at(1).sourcePts <= tickGateMs) {
                                auto old = m_frameQueue.dequeue();
                                av_frame_free(&old.frame);
                            }
                            // Count backstop (~10 s of frames) against
                            // future-stamped bursts after a re-anchor.
                            const int backstopFrames = 10 * m_targetFps;
                            while (m_frameQueue.size() > backstopFrames) {
                                auto old = m_frameQueue.dequeue();
                                av_frame_free(&old.frame);
                            }

                            // IMPORTANT: Unref the raw frame so the decoder can reuse the buffer
                            av_frame_unref(rawFrame);
                        }
                    }
                }
                // ── Audio packet handling ───────────────────────────────
                // Bug 1: the outer gate must NOT require swrCtx to be non-null.
                // swrCtx can be null because the INITIAL configureSwr failed
                // (e.g. sample_rate == 0 at codec-open for some parsers) or a
                // mid-stream rebuild failed.  If we gated on swrCtx here, no
                // future audio packet would ever enter the branch again and
                // audio would be dead for the rest of the connection.  Gate on
                // audioDecCtx only; the (re)build/retry lives INSIDE, reachable
                // whenever audio packets are still arriving.
                else if (pkt->stream_index == audioStreamIdx && audioDecCtx) {
                    if (avcodec_send_packet(audioDecCtx, pkt) >= 0) {
                        while (avcodec_receive_frame(audioDecCtx, audioFrame) >= 0) {
                            int64_t audioTs = audioFrame->pts;
                            if (audioTs == AV_NOPTS_VALUE)
                                audioTs = audioFrame->best_effort_timestamp;
                            if (audioTs == AV_NOPTS_VALUE) {
                                av_frame_unref(audioFrame);
                                continue;
                            }
                            const AVRational audioTb = inCtx->streams[audioStreamIdx]->time_base;

                            // Bug 1: (re)build the resampler BEFORE we use it,
                            // and make the retry reachable when swrCtx is null.
                            // Rebuild if swr was never built / a prior build
                            // failed (swrCtx == nullptr), OR this frame's input
                            // params differ from what swr was configured with
                            // (mid-stream sample-rate / channel-layout /
                            // sample-format change at ad breaks, 5.1->stereo,
                            // etc.).  Use the frame's actual sample_rate (guard
                            // >0; fall back to the decoder ctx) so a
                            // sample_rate-0-at-open case recovers once a real
                            // rate arrives.  On a stale resampler a layout
                            // shrink makes swr_convert read a NULL extended_data
                            // plane (segfault) and a rate change resamples at
                            // the wrong ratio (wrong pitch).
                            const int inRate = audioFrame->sample_rate > 0
                                                   ? audioFrame->sample_rate
                                                   : audioDecCtx->sample_rate;
                            const AVSampleFormat inFmt = (AVSampleFormat) audioFrame->format;
                            if (!swrCtx || inRate != swrInRate || inFmt != swrInFmt ||
                                av_channel_layout_compare(&audioFrame->ch_layout, &swrInLayout) !=
                                    0) {
                                if (swrCtx)
                                    qDebug()
                                        << "Source" << m_sourceIndex
                                        << "Audio format change mid-stream; rebuilding resampler";
                                configureSwr(inRate, inFmt, &audioFrame->ch_layout);
                            }
                            // Bug 1: if the resampler is STILL null after the
                            // (re)build attempt, skip just THIS frame; the next
                            // packet re-enters this branch and retries, so a
                            // transient failure is recoverable instead of
                            // permanently silencing audio.
                            if (!swrCtx) {
                                av_frame_unref(audioFrame);
                                continue;
                            }

                            // Bug 2: map audio onto the recording timeline using
                            // AUDIO'S OWN anchor and discontinuity detection,
                            // fully decoupled from the video anchor.  For RTMP
                            // (independent A/V timestamp domains) or any source
                            // where only video has a discontinuity, the old
                            // mapping (relMs = audioMs - containerStartMs, with
                            // containerStartMs set/re-anchored by VIDEO) shifted
                            // audio by seconds when video re-anchored but audio
                            // did not.  Here audio anchors on its own first
                            // packet and re-anchors on its own jumps (same
                            // >3000 ms-fwd / <-200 ms-back rule, on the audio
                            // timeline), so a video-only re-anchor cannot touch
                            // it.  On a normal MPEG-TS stream where A/V share
                            // the clock, audioAnchorStreamTimeMs samples
                            // m_sharedClock->elapsedMs() at ~the same first-
                            // packet moment as the video anchor, so end
                            // alignment is identical to before.
                            const int64_t nowMs = m_sharedClock->elapsedMs();
                            bool needAudioAnchor = (firstAudioDts == AV_NOPTS_VALUE);
                            if (!needAudioAnchor && prevAudioDts != AV_NOPTS_VALUE) {
                                const int64_t aDeltaMs =
                                    av_rescale_q(audioTs - prevAudioDts, audioTb, {1, 1000});
                                constexpr int64_t kForwardJumpMs = 3000;
                                constexpr int64_t kBackwardTolMs = -200;
                                if (aDeltaMs > kForwardJumpMs || aDeltaMs < kBackwardTolMs) {
                                    qDebug()
                                        << "Source" << m_sourceIndex << "Audio DTS discontinuity ("
                                        << aDeltaMs << "ms jump). Re-anchoring.";
                                    needAudioAnchor = true;
                                }
                            }
                            if (needAudioAnchor) {
                                firstAudioDts = audioTs;
                                audioAnchorStreamTimeMs = nowMs;
                            }
                            prevAudioDts = audioTs;

                            int64_t recPtsMs =
                                audioAnchorStreamTimeMs +
                                av_rescale_q(audioTs - firstAudioDts, audioTb, {1, 1000});

                            // Forward-sanity guard: if a missed jump still maps
                            // this sample absurdly far ahead of the recording
                            // clock, treat it as a discontinuity and re-anchor
                            // audio here rather than enqueuing a far-future
                            // sample (which would stall the FIFO write cursor).
                            if (recPtsMs > nowMs + 10000) {
                                qDebug() << "Source" << m_sourceIndex << "Audio sample" << recPtsMs
                                         << "ms far ahead of clock" << nowMs << "ms. Re-anchoring.";
                                firstAudioDts = audioTs;
                                audioAnchorStreamTimeMs = nowMs;
                                prevAudioDts = audioTs;
                                recPtsMs = nowMs;
                            }
                            if (recPtsMs < 0) { av_frame_unref(audioFrame); continue; }

                            // Resample to 48 kHz stereo S16
                            int outSamples = av_rescale_rnd(swr_get_delay(swrCtx, inRate) +
                                                                audioFrame->nb_samples,
                                                            48000, inRate, AV_ROUND_UP);
                            int outBufSize = av_samples_get_buffer_size(
                                nullptr, 2, outSamples, AV_SAMPLE_FMT_S16, 0);
                            uint8_t* outBuffer = (uint8_t*)av_malloc(outBufSize);
                            uint8_t* outBuf[1] = { outBuffer };

                            int converted = swr_convert(swrCtx,
                                outBuf, outSamples,
                                (const uint8_t**)audioFrame->extended_data, audioFrame->nb_samples);

                            if (converted > 0) {
                                // Stamp with the global recording timeline and
                                // hand off to the FIFO.  The master-pulse tick
                                // writes it to the muxer with the same jitter
                                // delay as video, sample-accurate and gap-filled,
                                // regardless of which view this source is in.
                                const int64_t startSample =
                                    recPtsMs * kAudioSampleRate / 1000;
                                enqueueAudio(startSample, outBuffer, converted);
                            }

                            av_freep(&outBuffer);
                            av_frame_unref(audioFrame);
                        }
                    }
                }
                av_packet_unref(pkt);
            } else if (readResult == AVERROR(EAGAIN)) {
                const int64_t lastPkt = m_lastPacketAtMs.load(std::memory_order_relaxed);
                if (m_connected && lastPkt >= 0
                    && m_monotonic.elapsed() - lastPkt > m_stallTimeoutMs) {
                    qDebug() << "Source" << m_sourceIndex << "Stalled stream. Restarting...";
                    break;
                }
                QThread::msleep(10);
            } else if (readResult == AVERROR(ETIMEDOUT) || readResult == AVERROR_EXIT) {
                qDebug() << "Source" << m_sourceIndex << "Timeout/Exit. Restarting...";
                break;
            } else if (readResult == AVERROR_EOF) {
                qDebug() << "Source" << m_sourceIndex << "End of stream. Restarting...";
                break;
            } else {
                qDebug() << "Source" << m_sourceIndex << "Read error (Disconnect).";
                break; // Break inner loop to trigger setupDecoder retry
            }
        }

        // CLEANUP INSIDE LOOP: Critical to prevent memory leaks on swap
        av_packet_free(&pkt);
        av_frame_free(&rawFrame);
        av_frame_free(&audioFrame);

        // Flush any remaining samples from the resampler into the FIFO
        // (continuation append at the FIFO end, -1 = "no timestamp").
        if (swrCtx) {
            int flushed = 1;
            while (flushed > 0) {
                uint8_t* flushBuf = (uint8_t*)av_malloc(4096);
                uint8_t* flushBufs[1] = { flushBuf };
                flushed = swr_convert(swrCtx, flushBufs, 1024, nullptr, 0);
                if (flushed > 0) enqueueAudio(-1, flushBuf, flushed);
                av_freep(&flushBuf);
            }
        }

        // Release the tracked input channel layout (Fix 4).  swrCtx itself is
        // freed by the detached close thread below; the layout copy is ours.
        av_channel_layout_uninit(&swrInLayout);

        // Detach the slow network close to a background thread.
        // avformat_close_input can block for seconds (SRT linger, RTMP
        // teardown), and protocol libraries may hold global locks that
        // stall OTHER workers' av_read_frame calls.
        AVCodecContext* decToFree = decCtx;
        AVCodecContext* audioDecToFree = audioDecCtx;
        SwrContext* swrToFree = swrCtx;
        AVFormatContext* fmtToClose = inCtx;
        decCtx = nullptr;
        audioDecCtx = nullptr;
        swrCtx = nullptr;
        inCtx = nullptr;
        if (fmtToClose) {
            // Install a static always-interrupt callback so the async close
            // can abort any internal I/O waits immediately (especially SRT's
            // recv/send during teardown).  A nullptr callback would let those
            // operations run to their full timeout while holding the SRT
            // global lock, blocking every other SRT connection.
            fmtToClose->interrupt_callback.callback = [](void*) -> int { return 1; };
            fmtToClose->interrupt_callback.opaque = nullptr;
        }
        // Fire-and-forget on a detached std::thread (NOT the shared global
        // pool, which the per-source captureLoops already occupy — queuing
        // closes there would let old contexts pile up unfreed during
        // reconnect cycles, leaking sockets and memory).  All four pointers
        // were moved into locals above and nulled out, so the worker never
        // touches them again; the close task owns them exclusively.  Each
        // fmtToClose carries its own always-interrupt callback, so the
        // detached thread cannot block indefinitely.
        std::thread([decToFree, audioDecToFree, swrToFree, fmtToClose]() mutable {
            if (decToFree) avcodec_free_context(&decToFree);
            if (audioDecToFree) avcodec_free_context(&audioDecToFree);
            if (swrToFree) swr_free(&swrToFree);
            if (fmtToClose) avformat_close_input(&fmtToClose);
        }).detach();

        setConnected(false);
    }

    m_captureRunning = false;
}

bool StreamWorker::setupDecoder(AVFormatContext** inCtx, AVCodecContext** decCtx, QUrl currentUrl, int* videoStreamIdx) {
    AVDictionary* opts = nullptr;
    const QString scheme = currentUrl.scheme().toLower();

    av_dict_set(&opts, "rw_timeout", "5000000", 0); // 5 second timeout for "stalled" streams
    av_dict_set(&opts, "timeout", "5000000", 0); // 5 second socket timeout (microseconds)
    av_dict_set(&opts, "recv_buffer_size", "15048000", 0);

    // Fix 1: low-latency probing.  By default avformat_find_stream_info
    // buffers seconds of the stream (large probesize/analyzeduration) and
    // then replays it, so the FIRST av_read_frame returns content that was
    // captured seconds ago.  captureLoop anchors anchorStreamTimeMs to
    // "now" on that first packet, baking the entire probe backlog in as a
    // constant latency — and since every source probes for a different
    // duration, multiview cameras desync by up to seconds.
    //
    // "nobuffer" tells the demuxer not to accumulate a replay buffer, and
    // the probesize/analyzeduration caps bound how much it reads to detect
    // the streams.  0.5 MB / 0.5 s is the widely-used low-latency live
    // setting and reliably detects H.264 + AAC; the anchor's backlog drops
    // from seconds to sub-100 ms.  These propagate from avformat_open_input
    // into find_stream_info, so a separate per-stream options array is not
    // needed (verified: the e2e H.264+AAC MPEG-TS streams still detect).
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "probesize", "500000", 0);       // ~0.5 MB
    av_dict_set(&opts, "analyzeduration", "500000", 0); // ~0.5 s (microseconds)

    if (scheme == "srt") {
        av_dict_set(&opts, "connect_timeout", "5000000", 0); // 5 second connect timeout
        // Increase SRT latency to smooth short network jitter (milliseconds)
        av_dict_set(&opts, "latency", "500", 0);
        av_dict_set(&opts, "rcvlatency", "500", 0);
        av_dict_set(&opts, "peerlatency", "500", 0);
        av_dict_set(&opts, "transtype", "live", 0);
        // Linger=0: on close, drop immediately instead of waiting to flush send
        // buffers.  SRT's default linger is 180 s and srt_close() holds a global
        // SRT library lock, so a closing socket (a source that never connected,
        // or any source at stopRecording) stalls teardown for minutes and blocks
        // every other SRT source meanwhile.  The engine is receive-only, so there
        // is nothing to flush.
        //
        // It MUST go in the URL query: SRT-private options set on the
        // avformat_open_input() dict do NOT propagate to the nested SRT
        // URLContext (only generic options like rw_timeout do), so a dict-set
        // "linger" is silently ignored and the 180 s default applies.
        QUrlQuery srtQuery(currentUrl);
        if (!srtQuery.hasQueryItem(QStringLiteral("linger")))
            srtQuery.addQueryItem(QStringLiteral("linger"), QStringLiteral("0"));
        currentUrl.setQuery(srtQuery);
    }

    if (scheme == "rtmp" || scheme == "rtmps") {
        av_dict_set(&opts, "rtmp_buffer", "5000", 0); // Buffer in milliseconds
        av_dict_set(&opts, "rtmp_live", "live", 0);
    }

    *inCtx = avformat_alloc_context();
    if (!*inCtx) {
        if (opts) av_dict_free(&opts);
        return false;
    }

    m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);

    (*inCtx)->interrupt_callback.callback = &StreamWorker::ffmpegInterruptCallback;
    (*inCtx)->interrupt_callback.opaque = this;

    int openResult = avformat_open_input(inCtx, currentUrl.toString().toUtf8().constData(), nullptr, &opts);
    if (openResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(openResult, errbuf, sizeof(errbuf));
        qDebug() << "Source" << m_sourceIndex << "avformat_open_input failed:" << errbuf;
        avformat_free_context(*inCtx);
        *inCtx = nullptr;
        if (opts) av_dict_free(&opts);
        return false;
    }

    // Keep blocking reads and rely on interrupt callback for stalls

    int infoResult = avformat_find_stream_info(*inCtx, nullptr);
    if (infoResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(infoResult, errbuf, sizeof(errbuf));
        qDebug() << "Source" << m_sourceIndex << "avformat_find_stream_info failed:" << errbuf;
        avformat_close_input(inCtx);
        if (opts) av_dict_free(&opts);
        return false;
    }

    const AVCodec* decoder = nullptr;
    int foundStreamIdx = av_find_best_stream(*inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (foundStreamIdx < 0) {
        qDebug() << "Source" << m_sourceIndex << "No video stream found.";
        avformat_close_input(inCtx);
        if (opts) av_dict_free(&opts);
        return false;
    }

    *decCtx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(*decCtx, (*inCtx)->streams[foundStreamIdx]->codecpar);

    if (opts) av_dict_free(&opts);
    int codecResult = avcodec_open2(*decCtx, decoder, nullptr);
    if (codecResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(codecResult, errbuf, sizeof(errbuf));
        qDebug() << "Source" << m_sourceIndex << "avcodec_open2 failed:" << errbuf;
        avcodec_free_context(decCtx);
        avformat_close_input(inCtx);
        return false;
    }
    if (videoStreamIdx) *videoStreamIdx = foundStreamIdx;
    return true;
}

bool StreamWorker::setupEncoder(AVCodecContext** encCtx) {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!encoder) return false;

    // MPEG-2's 12-bit horizontal_size_value/vertical_size_value cannot encode a
    // dimension that is a multiple of 4096 (the field would be 0).  avcodec_open2
    // would otherwise fail with a cryptic message AFTER the muxer header is
    // already on disk, leaving a stub .mkv.  Fail early with a clear diagnostic.
    if (m_targetWidth % 4096 == 0 || m_targetHeight % 4096 == 0) {
        qWarning() << "Source" << m_sourceIndex
                   << "MPEG-2 cannot encode a dimension that is a multiple of 4096"
                   << "(" << m_targetWidth << "x" << m_targetHeight
                   << ") — pick e.g. 3840x2160 instead of 4096x2160.";
        return false;
    }

    *encCtx = avcodec_alloc_context3(encoder);
    if (!*encCtx) return false;

    (*encCtx)->width = m_targetWidth;
    (*encCtx)->height = m_targetHeight;

    // MPEG-2 can only signal a small set of frame rates in its sequence header
    // (24/25/30/50/60 and the 1000/1001 variants, times a tiny n/d extension).
    // For any other integer fps the encoder silently writes the NEAREST
    // representable rate into the elementary stream while our container stamps
    // {fps,1}, so the file carries contradictory rates and ES-rate-trusting
    // tools mis-time the video.  Warn so the operator can pick a standard rate.
    switch (m_targetFps) {
    case 24:
    case 25:
    case 30:
    case 50:
    case 60:
        break; // exactly representable
    default:
        qWarning() << "Source" << m_sourceIndex << "fps" << m_targetFps
                   << "is not an exact MPEG-2 rate; the elementary stream will"
                   << "carry the nearest representable rate (use 24/25/30/50/60"
                   << "to avoid a container/ES rate mismatch).";
        break;
    }

    (*encCtx)->time_base = {1, m_targetFps};      // Internal codec clock
    (*encCtx)->framerate = {m_targetFps, 1};      // Target framerate

    (*encCtx)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*encCtx)->gop_size = 1;             // Keep Intra-only for seeking
    (*encCtx)->bit_rate = 30000000;

    m_latestFrame = av_frame_alloc();
    if (!m_latestFrame) return false;
    m_latestFrame->format = AV_PIX_FMT_YUV420P;
    m_latestFrame->width = m_targetWidth;
    m_latestFrame->height = m_targetHeight;
    if (av_frame_get_buffer(m_latestFrame, 0) < 0) return false; // Allocate actual pixel memory

    // Paint blue frame
    // Y plane (Brightness) - set to medium
    memset(m_latestFrame->data[0], 128, m_latestFrame->linesize[0] * m_latestFrame->height);
    // U plane (Blue Chrominance) - set to max
    memset(m_latestFrame->data[1], 255, m_latestFrame->linesize[1] * (m_latestFrame->height / 2));
    // V plane (Red Chrominance) - set to low
    memset(m_latestFrame->data[2], 107, m_latestFrame->linesize[2] * (m_latestFrame->height / 2));

    return avcodec_open2(*encCtx, encoder, nullptr) >= 0;
}

void StreamWorker::changeSource(const QString& newUrl) {
    {
        QMutexLocker locker(&m_urlMutex);
        if (m_url == newUrl) return; // No change
        m_url = newUrl;
    }

    if (newUrl.trimmed().isEmpty()) {
        m_paintBlue = 1;
        // Suppress capture-side enqueues until a real (non-empty) URL
        // connects again.  This stops a late straggler — a frame already
        // decoded from the now-cleared source — from being enqueued after
        // the tick clears the queue and paints blue, which would otherwise
        // overwrite the blue frame and then re-encode that stale frame
        // every tick forever (empty URL produces no fresh frames).
        m_suppressEnqueue.store(true, std::memory_order_relaxed);
    }

    m_restartCapture = 1;
}

// ─── Audio FIFO ─────────────────────────────────────────────────────────

void StreamWorker::enqueueAudio(int64_t startSample, const uint8_t* data, int numSamples) {
    if (numSamples <= 0) return;
    const qint64 numBytes = qint64(numSamples) * kAudioBytesPerSample;
    QMutexLocker locker(&m_audioFifoMutex);

    if (m_audioFifoStartSample < 0 || m_audioFifo.isEmpty()) {
        if (startSample < 0) return;  // continuation data with no stream yet
        m_audioFifoStartSample = startSample;
        m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
    } else {
        const int64_t expected = m_audioFifoStartSample
                                 + m_audioFifo.size() / kAudioBytesPerSample;
        const int64_t delta = (startSample < 0) ? 0 : startSample - expected;
        const int64_t jitterTol = kAudioSampleRate / 100;  // 10 ms

        if (qAbs(delta) <= jitterTol) {
            // Continuous (within PTS rounding jitter): plain append
            m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
        } else if (delta > 0) {
            // Gap (packet loss / reconnect): zero-fill so the track
            // stays sample-contiguous
            const int64_t maxFill = int64_t(kAudioSampleRate) * 10;
            if (delta > maxFill) {
                // Huge jump: restart the FIFO at the new position
                m_audioFifo.clear();
                m_audioFifoStartSample = startSample;
            } else {
                m_audioFifo.append(QByteArray(int(delta * kAudioBytesPerSample), '\0'));
            }
            m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
        } else {
            // Overlap: drop the part we already have
            const int64_t drop = -delta;
            if (drop >= numSamples) return;
            m_audioFifo.append(reinterpret_cast<const char*>(data) + drop * kAudioBytesPerSample,
                               numBytes - drop * kAudioBytesPerSample);
        }
    }

    // Cap the FIFO at ~10 s
    const int maxBytes = kAudioSampleRate * 10 * kAudioBytesPerSample;
    if (m_audioFifo.size() > maxBytes) {
        const int excess = m_audioFifo.size() - maxBytes;
        m_audioFifo.remove(0, excess);
        m_audioFifoStartSample += excess / kAudioBytesPerSample;
    }
}

void StreamWorker::writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs) {
    // Audio shares the video path's jitter delay so both land on the
    // same timeline: a video frame written at file-time T shows source
    // content from T - jitter.  The cursor runs on the FILE timeline;
    // the FIFO holds source-timeline samples, so file position P maps
    // to FIFO position P - jitter.
    const int64_t jitterSamples = int64_t(kJitterBufferMs) * kAudioSampleRate / 1000;
    const int64_t trimSamples = trimMs * kAudioSampleRate / 1000;
    const int64_t targetEnd = recordingTimeMs * kAudioSampleRate / 1000;
    if (targetEnd <= 0) return;

    if (track < 0) {
        // Not mapped to a view: discard consumed FIFO data and keep the
        // cursor pinned to "now" so mapping in resumes at current time.
        m_audioWriteCursor = targetEnd;
        QMutexLocker locker(&m_audioFifoMutex);
        if (m_audioFifoStartSample >= 0) {
            const int64_t dropSamples =
                (targetEnd - jitterSamples - trimSamples) - m_audioFifoStartSample;
            if (dropSamples > 0) {
                const int dropBytes = int(qMin<int64_t>(dropSamples * kAudioBytesPerSample,
                                                        m_audioFifo.size()));
                m_audioFifo.remove(0, dropBytes);
                m_audioFifoStartSample += dropBytes / kAudioBytesPerSample;
            }
        }
        return;
    }

    if (m_audioWriteCursor < 0) {
        m_audioWriteCursor = qMax<int64_t>(0, targetEnd - kAudioSampleRate / m_targetFps);
    }
    if (targetEnd <= m_audioWriteCursor) return;

    // Catch up at most 1 s per tick: the track stays contiguous, a large
    // backlog (stalled event loop) just drains over several ticks.
    const int64_t n = qMin<int64_t>(targetEnd - m_audioWriteCursor, kAudioSampleRate);
    const int64_t start = m_audioWriteCursor;                     // file timeline
    const int64_t srcStart = start - jitterSamples - trimSamples; // source timeline (+trim)

    QByteArray chunk(int(n * kAudioBytesPerSample), '\0');
    {
        QMutexLocker locker(&m_audioFifoMutex);
        if (m_audioFifoStartSample >= 0 && !m_audioFifo.isEmpty()) {
            const int64_t fifoStart = m_audioFifoStartSample;
            const int64_t fifoEnd = fifoStart + m_audioFifo.size() / kAudioBytesPerSample;
            const int64_t copyFrom = qMax(srcStart, fifoStart);
            const int64_t copyTo = qMin(srcStart + n, fifoEnd);
            if (copyTo > copyFrom) {
                memcpy(chunk.data() + (copyFrom - srcStart) * kAudioBytesPerSample,
                       m_audioFifo.constData() + (copyFrom - fifoStart) * kAudioBytesPerSample,
                       size_t((copyTo - copyFrom) * kAudioBytesPerSample));
            }
            // Trim everything we just consumed (or skipped past)
            const int64_t dropSamples = (srcStart + n) - fifoStart;
            if (dropSamples > 0) {
                const int dropBytes = int(qMin<int64_t>(dropSamples * kAudioBytesPerSample,
                                                        m_audioFifo.size()));
                m_audioFifo.remove(0, dropBytes);
                m_audioFifoStartSample += dropBytes / kAudioBytesPerSample;
            }
        }
    }
    m_audioWriteCursor = start + n;

    const int audioTrackIdx = m_muxer->audioTrackOffset() + track;
    AVStream* st = m_muxer->getStream(audioTrackIdx);
    if (!st) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    if (av_new_packet(pkt, chunk.size()) == 0) {
        memcpy(pkt->data, chunk.constData(), size_t(chunk.size()));
        pkt->stream_index = audioTrackIdx;
        pkt->pts = av_rescale_q(start, {1, kAudioSampleRate}, st->time_base);
        pkt->dts = pkt->pts;
        pkt->duration = av_rescale_q(n, {1, kAudioSampleRate}, st->time_base);
        m_muxer->writePacket(pkt);
    }
    av_packet_free(&pkt);
}
