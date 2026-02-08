#include "streamworker.h"
#include <QDebug>
#include <QDateTime>

StreamWorker::StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock *clock,
                           int targetWidth, int targetHeight, int targetFps, QObject* parent)
    : QThread(parent), m_url(url), m_sourceIndex(sourceIndex), m_viewTrack(-1), m_muxer(muxer), m_sharedClock(clock) {
    m_restartCapture = 0;
    m_internalFrameCount = 0;
    if (targetWidth > 0) m_targetWidth = targetWidth;
    if (targetHeight > 0) m_targetHeight = targetHeight;
    if (targetFps > 0) m_targetFps = targetFps;
}

StreamWorker::~StreamWorker() { stop(); wait(); }

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
    if (m_connected && m_lastPacketTimer.isValid() && m_lastPacketTimer.elapsed() > m_stallTimeoutMs) return true;
    return false;
}

void StreamWorker::run() {
    // 1. Setup the persistent encoder context
    if (!setupEncoder(&m_persistentEncCtx)) return;

    // 2. Enter the event loop. The thread stays alive, waiting for
    // signals (masterPulse) or concurrent tasks (captureLoop).
    exec();
    m_captureFuture.waitForFinished();

    // Cleanup when exec() returns (on stop)
    avcodec_free_context(&m_persistentEncCtx);

    while(!m_frameQueue.isEmpty()){
        auto qf = m_frameQueue.dequeue();
        av_frame_free(&qf.frame);
    }
}

void StreamWorker::onMasterPulse(int64_t frameIndex, int64_t streamTimeMs) {
    m_internalFrameCount = frameIndex;

    if (!m_persistentEncCtx) return;

    // Stall detection: if connected but no frames for too long, signal restart.
    if (m_captureRunning && m_connected && m_lastFrameEnqueueTimer.isValid()
        && m_lastFrameEnqueueTimer.elapsed() > m_stallTimeoutMs) {
        qDebug() << "Source" << m_sourceIndex << "No frames queued. Forcing restart...";
        m_restartCapture = 1;
    }

    // Launch captureLoop only if it truly isn't running.
    // Never kill a running captureLoop from here — it handles restarts itself.
    if (!m_captureRunning && !m_captureFuture.isRunning()) {
        m_restartCapture = 0;
        m_captureRunning = true;
        m_captureFuture = QtConcurrent::run([this]() {
            this->captureLoop();
        });
    }

    processEncoderTick(m_persistentEncCtx, streamTimeMs);
}

void StreamWorker::processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs) {
    AVPacket* outPkt = av_packet_alloc();
    bool havePacket = false;
    int track = -1;

    {
        QMutexLocker locker(&m_frameMutex);

        // Handle deferred blue paint from changeSource
        if (m_paintBlue.fetchAndStoreRelaxed(0)) {
            while (!m_frameQueue.isEmpty()) {
                auto qf = m_frameQueue.dequeue();
                av_frame_free(&qf.frame);
            }
            if (m_latestFrame && m_latestFrame->data[0]) {
                memset(m_latestFrame->data[0], 128, m_latestFrame->linesize[0] * m_latestFrame->height);
                memset(m_latestFrame->data[1], 255, m_latestFrame->linesize[1] * (m_latestFrame->height / 2));
                memset(m_latestFrame->data[2], 107, m_latestFrame->linesize[2] * (m_latestFrame->height / 2));
            }
        }

        // ALWAYS do jitter pull to keep m_latestFrame fresh, even when
        // not assigned to a view.  This ensures frames are ready the
        // instant this source gets mapped to a view.
        int64_t currentRecordingTimeMs = (m_internalFrameCount * 1000) / m_targetFps;
        int64_t targetTimeMs = currentRecordingTimeMs - m_jitterBufferMs;
        if (targetTimeMs < 0) targetTimeMs = 0;

        while (!m_frameQueue.isEmpty() && m_frameQueue.head().sourcePts <= targetTimeMs) {
            QueuedFrame top = m_frameQueue.dequeue();
            av_frame_unref(m_latestFrame);
            av_frame_move_ref(m_latestFrame, top.frame);
            av_frame_free(&top.frame);
        }

        // Read the current view-track assignment (atomic, set by UIManager).
        // -1 = this source is not assigned to any view, skip encoding.
        track = m_viewTrack.load(std::memory_order_relaxed);

        if (track >= 0 && m_latestFrame && m_latestFrame->data[0]) {
            // Set PTS on the FRAME, not the packet. avcodec_receive_packet
            // overwrites the packet entirely, so any PTS/DTS set on it
            // before encoding is lost.  The encoder propagates the frame
            // PTS into the output packet, keeping it in sync with the
            // global frame counter even after gaps (source was unmapped).
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
    } // m_frameMutex released BEFORE the muxer write

    if (havePacket) {
        m_muxer->writePacket(outPkt);
    }
    av_packet_free(&outPkt);
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
            m_connected = false;
            while (m_captureRunning && !m_restartCapture) {
                QThread::msleep(100);
            }
            continue;
        }

        qDebug() << "Source" << m_sourceIndex << "Attempting connection to:" << currentUrl;
        m_connected = false;

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
        m_connected = true;
        m_connectBackoffMs = 1000;

        if (!m_sharedClock) {
            qDebug() << "Source" << m_sourceIndex << "No shared clock. Restarting...";
            m_restartCapture = 1;
            m_captureRunning = false;
            break;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* rawFrame = av_frame_alloc();
        if (!pkt || !rawFrame) {
            qDebug() << "Source" << m_sourceIndex << "Failed to allocate packet/frame.";
            if (pkt) av_packet_free(&pkt);
            if (rawFrame) av_frame_free(&rawFrame);
            avcodec_free_context(&decCtx);
            avformat_close_input(&inCtx);
            continue;
        }

        int firstSourcePts = -1;
        int64_t anchorStreamTimeMs = -1;
        int64_t firstPacketDts = -1;

        m_lastPacketTimer.restart();
        m_lastFrameEnqueueTimer.restart();

        while (m_captureRunning && !m_restartCapture) {
            int readResult = av_read_frame(inCtx, pkt);
            if (readResult >= 0) {
                m_lastPacketTimer.restart();
                if (pkt->stream_index == videoStreamIdx) {

                    int64_t pktDts = pkt->dts;
                    if (pktDts == AV_NOPTS_VALUE) pktDts = pkt->pts;
                    if (pktDts == AV_NOPTS_VALUE) {
                        av_packet_unref(pkt);
                        continue;
                    }

                    // 1. Establish the Anchor on the very first packet of this URL session
                    if (firstPacketDts == -1) {
                        firstPacketDts = pktDts;
                        // Where are we in the global recording right now?
                        anchorStreamTimeMs = m_sharedClock->elapsedMs();
                    }

                    if (avcodec_send_packet(decCtx, pkt) >= 0) {
                        while (avcodec_receive_frame(decCtx, rawFrame) >= 0) {

                            // 1. Initialize start time correctly using the packet DTS
                            if (firstSourcePts == -1) firstSourcePts = pktDts;

                            // 2. Prepare the container for the scaled frame
                            QueuedFrame qf;
                            qf.frame = av_frame_alloc();
                            qf.frame->format = AV_PIX_FMT_YUV420P;
                            qf.frame->width = m_targetWidth;
                            qf.frame->height = m_targetHeight;
                            av_frame_get_buffer(qf.frame, 0);

                            // 3. Scale
                            m_swsCtx = sws_getCachedContext(m_swsCtx,
                                                            rawFrame->width, rawFrame->height, (AVPixelFormat)rawFrame->format,
                                                            m_targetWidth, m_targetHeight, AV_PIX_FMT_YUV420P,
                                                            SWS_BICUBIC, nullptr, nullptr, nullptr);

                            sws_scale(m_swsCtx, rawFrame->data, rawFrame->linesize, 0, rawFrame->height,
                                      qf.frame->data, qf.frame->linesize);

                            // 2. Calculate the RELATIVE offset of this packet in its own stream
                            int64_t relativeMs = av_rescale_q(pktDts - firstPacketDts,
                                                              inCtx->streams[videoStreamIdx]->time_base,
                                                              {1, 1000});

                            // 3. Map it to the Global Timeline
                            // If a burst of 10 frames arrives, relativeMs increases by 33ms each,
                            // spacing them out perfectly in our queue.
                            qf.sourcePts = anchorStreamTimeMs + relativeMs;

                            QMutexLocker locker(&m_frameMutex);
                            m_frameQueue.enqueue(qf);

                            m_lastFrameEnqueueTimer.restart();

                            while (m_frameQueue.size() > 1000) {
                                auto old = m_frameQueue.dequeue();
                                av_frame_free(&old.frame);
                            }

                            // IMPORTANT: Unref the raw frame so the decoder can reuse the buffer
                            av_frame_unref(rawFrame);
                        }
                    }
                }
                av_packet_unref(pkt);
            } else if (readResult == AVERROR(EAGAIN)) {
                if (m_connected && m_lastPacketTimer.isValid() && m_lastPacketTimer.elapsed() > m_stallTimeoutMs) {
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

        // Detach the slow network close to a background thread.
        // avformat_close_input can block for seconds (SRT linger, RTMP
        // teardown), and protocol libraries may hold global locks that
        // stall OTHER workers' av_read_frame calls.
        AVCodecContext* decToFree = decCtx;
        AVFormatContext* fmtToClose = inCtx;
        decCtx = nullptr;
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
        QThreadPool::globalInstance()->start([decToFree, fmtToClose]() mutable {
            if (decToFree) avcodec_free_context(&decToFree);
            if (fmtToClose) avformat_close_input(&fmtToClose);
        });

        m_connected = false;
    }

    m_captureRunning = false;
}

bool StreamWorker::setupDecoder(AVFormatContext** inCtx, AVCodecContext** decCtx, QUrl currentUrl, int* videoStreamIdx) {
    AVDictionary* opts = nullptr;
    const QString scheme = currentUrl.scheme().toLower();

    av_dict_set(&opts, "rw_timeout", "5000000", 0); // 5 second timeout for "stalled" streams
    av_dict_set(&opts, "timeout", "5000000", 0); // 5 second socket timeout (microseconds)
    av_dict_set(&opts, "recv_buffer_size", "15048000", 0);

    if (scheme == "srt") {
        av_dict_set(&opts, "connect_timeout", "5000000", 0); // 5 second connect timeout
        // Increase SRT latency to smooth short network jitter (milliseconds)
        av_dict_set(&opts, "latency", "500", 0);
        av_dict_set(&opts, "rcvlatency", "500", 0);
        av_dict_set(&opts, "peerlatency", "500", 0);
        av_dict_set(&opts, "transtype", "live", 0);
        // Linger=0: on close, drop immediately instead of waiting to
        // flush send buffers.  srt_close() holds a global SRT library
        // lock, so any linger stalls ALL other SRT sockets' reads.
        av_dict_set(&opts, "linger", "0", 0);
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

    m_lastPacketTimer.restart();

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
    *encCtx = avcodec_alloc_context3(encoder);
    if (!*encCtx) return false;

    (*encCtx)->width = m_targetWidth;
    (*encCtx)->height = m_targetHeight;

    // --- CHANGE THIS: Use a standard MPEG-2 rate ---
    (*encCtx)->time_base = {1, m_targetFps};      // Internal codec clock
    (*encCtx)->framerate = {m_targetFps, 1};      // Target framerate
    // -----------------------------------------------

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
    }

    m_restartCapture = 1;
}
