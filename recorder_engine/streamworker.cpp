#include "streamworker.h"
#include "ingest/ffmpegingestsession.h"
#include "ingest/ingestsession.h"
#include "ingest/rtmpprotocol.h"
#if defined(OLR_NATIVE_SRT_AVAILABLE)
#include "ingest/nativesrtingestsession.h"
#endif
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
#include "ingest/nativertmpingestsession.h"
#endif
#include <QDebug>
#include <QDateTime>
#include <QUrl>
#include <QtGlobal>

#include <memory>

namespace {
QString ingestFailureKindForLog(IngestFailureKind failure) {
    switch (failure) {
    case IngestFailureKind::UnsupportedProfile:
        return QStringLiteral("unsupported profile");
    case IngestFailureKind::DecodeCapability:
        return QStringLiteral("decode capability failure");
    case IngestFailureKind::MalformedStream:
        return QStringLiteral("malformed stream");
    case IngestFailureKind::TransientNetwork:
        return QStringLiteral("transient network failure");
    case IngestFailureKind::None:
        break;
    }
    return QStringLiteral("unknown failure");
}
} // namespace

StreamWorker::StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock* clock,
                           int targetWidth, int targetHeight, int targetFps, QObject* parent)
    : QThread(parent), m_url(url), m_sourceIndex(sourceIndex), m_viewTrack(-1), m_muxer(muxer),
      m_sharedClock(clock) {
    qRegisterMetaType<SrtStats>("SrtStats");
    m_restartCapture = 0;
    m_internalFrameCount = 0;
    m_monotonic.start();
    if (targetWidth > 0) m_targetWidth = targetWidth;
    if (targetHeight > 0) m_targetHeight = targetHeight;
    if (targetFps > 0) m_targetFps = targetFps;
}

StreamWorker::~StreamWorker() {
    stop();
    wait();
}

void StreamWorker::setConnected(bool c) {
    // exchange first so the emit fires exactly once per real transition,
    // even if two capture-thread call sites race the same value.
    const bool prev = m_connected.exchange(c, std::memory_order_relaxed);
    if (prev != c) {
        emit connectionChanged(m_sourceIndex, c);
    }
}

void debugTimestamp(const QString& prefix, int trackIndex) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    qDebug() << "[" << timeStr << "] [Track" << trackIndex << "]" << prefix;
}

void StreamWorker::stop() {
    m_restartCapture = 1;
    m_captureRunning = false;
    this->quit();
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

    while (!m_frameQueue.isEmpty()) {
        auto qf = m_frameQueue.dequeue();
        av_frame_free(&qf.frame);
    }
}

void StreamWorker::onMasterPulse(int64_t frameIndex, int64_t streamTimeMs) {
    m_internalFrameCount = frameIndex;

    // Snapshot the trim once per pulse so video and audio of this tick use the
    // SAME offset even if the UI thread changes it mid-pulse (keeps A/V locked).
    const int64_t trimMs = m_trimOffsetMs.load(std::memory_order_relaxed);

    // Snapshot the jitter window once per pulse too, so video + audio of this tick
    // share one value even if a URL change flips it mid-pulse (keeps A/V locked).
    const int64_t jitterMs = m_activeJitterWindowMs.load(std::memory_order_relaxed);

    // Publish this tick's jitter-pull gate for the capture thread's
    // queue pre-drain (see captureLoop).
    m_lastTickTargetMs.store(
        qMax<int64_t>(0, (frameIndex * 1000) / m_targetFps - jitterMs - trimMs),
        std::memory_order_relaxed);

    if (!m_persistentEncCtx) return;

    // Stall detection: if connected but no frames for too long, signal restart.
    const int64_t lastEnq = m_lastFrameEnqueueAtMs.load(std::memory_order_relaxed);
    if (m_captureRunning && m_connected && lastEnq >= 0 &&
        m_monotonic.elapsed() - lastEnq > m_stallTimeoutMs) {
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

    processEncoderTick(m_persistentEncCtx, streamTimeMs, trimMs, jitterMs);
}

void StreamWorker::processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs, int64_t trimMs,
                                      int64_t jitterMs) {
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
        int64_t targetTimeMs = currentRecordingTimeMs - jitterMs - trimMs;
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
        memset(m_latestFrame->data[2], 107,
               m_latestFrame->linesize[2] * (m_latestFrame->height / 2));
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
        {
            QMutexLocker locker(&m_metadataMutex);
            metaJson = m_sourceMetadataJson;
        }
        if (!metaJson.isEmpty()) {
            m_muxer->writeMetadataPacket(track, streamTimeMs, metaJson);
        }
    }
    av_packet_free(&outPkt);

    // Write this tick's worth of audio for the assigned view track
    // (sample-accurate cursor, silence-filled where capture had nothing).
    writeAudioForTick(currentRecordingTimeMs, track, trimMs, jitterMs);
}

void StreamWorker::captureLoop() {
    bool suppressNativeForCurrentUrl = false;
    QString nativeSuppressedUrl;
    QString forceFfmpegUrl;
    bool forceFfmpegForCurrentUrl = false;

    while (m_captureRunning) {
        // If a restart was requested (e.g. changeSource), acknowledge it
        // and loop back to re-read the URL instead of exiting.
        m_restartCapture = 0;

        QString currentUrl;
        {
            QMutexLocker locker(&m_urlMutex);
            currentUrl = m_url;
        }

        {
            // Right-size the jitter window for this source's transport. SRT (native
            // or ffmpeg) pre-buffers via TSBPD, so it needs only a small floor.
            int srtFloor = kSrtJitterFloorMs;
            const int envFloor = qEnvironmentVariableIntValue("OLR_SRT_JITTER_MS");
            if (envFloor > 0) srtFloor = envFloor;
            m_activeJitterWindowMs.store(
                jitterWindowMs(QUrl(currentUrl).scheme().toLower(), srtFloor, kJitterBufferMs),
                std::memory_order_relaxed);
        }

        if (nativeSuppressedUrl != currentUrl) {
            nativeSuppressedUrl = currentUrl;
            suppressNativeForCurrentUrl = false;
        }

        if (forceFfmpegUrl != currentUrl) {
            forceFfmpegUrl = currentUrl;
            forceFfmpegForCurrentUrl = false;
        }

        // If URL is empty, don't attempt to connect. Just idle until
        // a new URL is set via changeSource() which sets m_restartCapture.
        if (currentUrl.trimmed().isEmpty()) {
            setConnected(false);
            while (m_captureRunning && !m_restartCapture) {
                QThread::msleep(100);
            }
            continue;
        }

        if (forceFfmpegForCurrentUrl) {
            qDebug() << "Source" << m_sourceIndex
                     << "Attempting FFmpeg fallback connection for current URL.";
        } else {
            qDebug() << "Source" << m_sourceIndex
                     << "Attempting connection to:"
                     << RtmpUrlParts::redactedForLog(QUrl(currentUrl));
        }
        setConnected(false);

        IngestCallbacks callbacks;
        callbacks.shouldStop = [this]() {
            return !m_captureRunning.load(std::memory_order_relaxed) ||
                   m_restartCapture.loadRelaxed() != 0;
        };
        callbacks.recordingClockMs = [this]() -> int64_t {
            return m_sharedClock ? m_sharedClock->elapsedMs() : -1;
        };
        callbacks.logInfo = [this](const QString& message) {
            qDebug() << "Source" << m_sourceIndex << message;
        };
        callbacks.onVideoFrame = [this](DecodedVideoFrame decoded) {
            if (!decoded.frame) return;

            // Bug 4: a restart/blue-paint is pending (source was cleared to an
            // empty URL). Drop frames from the old source so a late straggler
            // can't overwrite the painted blue frame.
            if (m_suppressEnqueue.load(std::memory_order_relaxed)) {
                av_frame_free(&decoded.frame);
                return;
            }

            QueuedFrame qf;
            qf.frame = decoded.frame;
            qf.sourcePts = decoded.sourcePtsMs;

            QMutexLocker locker(&m_frameMutex);
            m_frameQueue.enqueue(qf);

            m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);

            // Pre-drain frames the next tick would discard anyway: the tick
            // keeps only the newest frame at-or-before its gate, so the head is
            // garbage as soon as a SECOND frame is inside the gate.
            const int64_t tickGateMs = m_lastTickTargetMs.load(std::memory_order_relaxed);
            while (tickGateMs >= 0 && m_frameQueue.size() >= 2 &&
                   m_frameQueue.at(1).sourcePts <= tickGateMs) {
                auto old = m_frameQueue.dequeue();
                av_frame_free(&old.frame);
            }

            // Count backstop (~10 s of frames) against future-stamped bursts
            // after a re-anchor.
            const int backstopFrames = 10 * m_targetFps;
            while (m_frameQueue.size() > backstopFrames) {
                auto old = m_frameQueue.dequeue();
                av_frame_free(&old.frame);
            }
        };
        callbacks.onAudioChunk = [this](DecodedAudioChunk chunk) {
            enqueueAudio(chunk.startSample,
                         reinterpret_cast<const uint8_t*>(chunk.pcmS16Stereo.constData()),
                         chunk.pcmS16Stereo.size() / kAudioBytesPerSample);
        };
        callbacks.setConnected = [this](bool connected) {
            setConnected(connected);
        };
        callbacks.reportStats = [this](const SrtStats& stats) {
            emit statsUpdated(m_sourceIndex, stats);
        };

        const QUrl sourceUrl(currentUrl);
        bool nativeSrtAvailable = false;
#if defined(OLR_NATIVE_SRT_AVAILABLE)
        nativeSrtAvailable = NativeSrtIngestSession::supportsUrl(sourceUrl);
#endif
        bool nativeRtmpAvailable = false;
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
        nativeRtmpAvailable = NativeRtmpIngestSession::supportsUrl(sourceUrl);
#endif
        IngestBackendOptions backendOptions =
            ingestBackendOptionsFromEnvironment(sourceUrl, nativeSrtAvailable, nativeRtmpAvailable);
        if (forceFfmpegForCurrentUrl) {
            backendOptions.preferNativeRtmp = false;
        }
        const IngestBackendKind backendKind = selectIngestBackend(sourceUrl, backendOptions);
        const bool nativeRtmpAttempt = backendKind == IngestBackendKind::NativeRtmp;
#if !defined(OLR_NATIVE_SRT_AVAILABLE) && !defined(OLR_NATIVE_RTMP_AVAILABLE)
        Q_UNUSED(backendKind);
#endif

        std::unique_ptr<IngestSession> session;
#if defined(OLR_NATIVE_SRT_AVAILABLE)
        if (backendKind == IngestBackendKind::NativeSrt) {
            session = std::make_unique<NativeSrtIngestSession>(m_sourceIndex, m_targetWidth,
                                                               m_targetHeight, &m_captureRunning);
        } else
#endif
#if defined(OLR_NATIVE_RTMP_AVAILABLE)
            if (backendKind == IngestBackendKind::NativeRtmp) {
            session = std::make_unique<NativeRtmpIngestSession>(m_sourceIndex, m_targetWidth,
                                                                m_targetHeight, &m_captureRunning);
        } else
#endif
        {
            session = std::make_unique<FfmpegIngestSession>(m_sourceIndex, m_targetWidth,
                                                            m_targetHeight, m_targetFps);
        }

        if (!session->open(sourceUrl, callbacks)) {
            const IngestFailureKind failureKind = session->lastFailureKind();
            if (nativeRtmpAttempt && shouldFallbackToFfmpegAfterNativeFailure(failureKind)) {
                if (qEnvironmentVariableIsSet("OLR_NATIVE_RTMP_DISABLE_FALLBACK")) {
                    qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with"
                             << ingestFailureKindForLog(failureKind)
                             << "and fallback is disabled; stopping capture for this URL.";
                    m_captureRunning = false;
                    break;
                }
                qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with"
                         << ingestFailureKindForLog(failureKind)
                         << "; retrying FFmpeg for this URL.";
                forceFfmpegForCurrentUrl = true;
                m_connectBackoffMs = 1000;
                continue;
            }
            if (forceFfmpegForCurrentUrl && backendKind == IngestBackendKind::Ffmpeg) {
                qDebug() << "Source" << m_sourceIndex
                         << "FFmpeg fallback open failed. Retrying in"
                         << (m_connectBackoffMs / 1000.0) << "s...";
            } else {
                qDebug() << "Source" << m_sourceIndex << "Connect failed. Retrying in"
                         << (m_connectBackoffMs / 1000.0) << "s...";
            }
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
        m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
        // A real source connected: stragglers from any previously-cleared
        // source are gone, so resume enqueuing frames for this new URL.
        m_suppressEnqueue.store(false, std::memory_order_relaxed);

        if (!m_sharedClock) {
            qDebug() << "Source" << m_sourceIndex << "No shared clock. Restarting...";
            m_restartCapture = 1;
            m_captureRunning = false;
            setConnected(false);
            break;
        }

        session->run();

        const QString nativeFallbackReason = session->nativeFallbackReason();
        if (!nativeFallbackReason.isEmpty()) {
            qDebug() << "Source" << m_sourceIndex
                     << "Native ingest fallback requested:" << nativeFallbackReason
                     << "Retrying with FFmpeg for this URL.";
            suppressNativeForCurrentUrl = true;
        }

        setConnected(false);
        const IngestFailureKind failureKind = session->lastFailureKind();
        if (nativeRtmpAttempt && shouldFallbackToFfmpegAfterNativeFailure(failureKind)) {
            if (qEnvironmentVariableIsSet("OLR_NATIVE_RTMP_DISABLE_FALLBACK")) {
                qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with"
                         << ingestFailureKindForLog(failureKind)
                         << "and fallback is disabled; stopping capture for this URL.";
                m_captureRunning = false;
                break;
            }
            qDebug() << "Source" << m_sourceIndex << "Native RTMP failed with"
                     << ingestFailureKindForLog(failureKind)
                     << "; retrying FFmpeg for this URL.";
            forceFfmpegForCurrentUrl = true;
            m_connectBackoffMs = 1000;
            continue;
        }
    }

    m_captureRunning = false;
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

    (*encCtx)->time_base = {1, m_targetFps}; // Internal codec clock
    (*encCtx)->framerate = {m_targetFps, 1}; // Target framerate

    (*encCtx)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*encCtx)->gop_size = 1; // Keep Intra-only for seeking
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
        if (startSample < 0) return; // continuation data with no stream yet
        m_audioFifoStartSample = startSample;
        m_audioFifo.append(reinterpret_cast<const char*>(data), numBytes);
    } else {
        const int64_t expected = m_audioFifoStartSample + m_audioFifo.size() / kAudioBytesPerSample;
        const int64_t delta = (startSample < 0) ? 0 : startSample - expected;
        const int64_t jitterTol = kAudioSampleRate / 100; // 10 ms

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

void StreamWorker::writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs,
                                     int64_t jitterMs) {
    // Audio shares the video path's jitter delay so both land on the
    // same timeline: a video frame written at file-time T shows source
    // content from T - jitter.  The cursor runs on the FILE timeline;
    // the FIFO holds source-timeline samples, so file position P maps
    // to FIFO position P - jitter.
    const int64_t jitterSamples = jitterMs * kAudioSampleRate / 1000;
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
                const int dropBytes =
                    int(qMin<int64_t>(dropSamples * kAudioBytesPerSample, m_audioFifo.size()));
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
                const int dropBytes =
                    int(qMin<int64_t>(dropSamples * kAudioBytesPerSample, m_audioFifo.size()));
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
