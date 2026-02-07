#include "streamworker.h"
#include <QDebug>
#include <QDateTime>

StreamWorker::StreamWorker(const QString& url, int trackIndex, Muxer* muxer, RecordingClock *clock, QObject* parent)
    : QThread(parent), m_url(url), m_trackIndex(trackIndex), m_muxer(muxer), m_sharedClock(clock) {}

StreamWorker::~StreamWorker() { stop(); wait(); }

void debugTimestamp(const QString &prefix, int trackIndex) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    qDebug() << "[" << timeStr << "] [Track" << trackIndex << "]" << prefix;
}

void StreamWorker::stop() { m_captureRunning = false; this->quit(); }

int StreamWorker::ffmpegInterruptCallback(void* opaque) {
    StreamWorker* worker = static_cast<StreamWorker*>(opaque);
    return worker ? worker->shouldInterrupt() : 0;
}

bool StreamWorker::shouldInterrupt() const {
    if (!m_captureRunning || m_restartCapture) return true;
    if (m_lastPacketTimer.isValid() && m_lastPacketTimer.elapsed() > m_stallTimeoutMs) return true;
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
    // This runs on the StreamWorker's thread thanks to QueuedConnection
    m_internalFrameCount = frameIndex;

    if (!m_persistentEncCtx) return;

    if (m_restartCapture || !m_captureRunning) {
        if (m_captureFuture.isRunning()) {
            // A capture is running. Signal it to stop, and wait for the next pulse to restart.
            m_captureRunning = false;
        } else {
            // No capture is running, so let's start one.
            m_restartCapture = 0;
            m_captureRunning = true;
            m_captureFuture = QtConcurrent::run([this]() {
                this->captureLoop();
            });
        }
    }

    processEncoderTick(m_persistentEncCtx, streamTimeMs);
}

void StreamWorker::processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs) {
    QMutexLocker locker(&m_frameMutex);

    // Theoretical time for this frame in the master timeline
    int64_t currentRecordingTimeMs = (m_internalFrameCount * 1000) / 30;

    // Jitter buffer pull...
    while (!m_frameQueue.isEmpty() && m_frameQueue.head().sourcePts <= currentRecordingTimeMs) {
        QueuedFrame top = m_frameQueue.dequeue();
        av_frame_unref(m_latestFrame);
        av_frame_move_ref(m_latestFrame, top.frame);
        av_frame_free(&top.frame);
    }

    AVPacket* outPkt = av_packet_alloc();
    // Encode whatever is in m_latestFrame (CFR logic)
    if (m_latestFrame && m_latestFrame->data[0]) {
        outPkt->pts = outPkt->dts = m_internalFrameCount; // 0, 1, 2, 3...
        outPkt->duration = 1;

        if (avcodec_send_frame(encCtx, m_latestFrame) == 0) {
            while (avcodec_receive_packet(encCtx, outPkt) == 0) {
                outPkt->stream_index = m_trackIndex;

                AVStream* st = m_muxer->getStream(m_trackIndex);
                // Rescale from {1, 30} to Muxer Stream's {1, 30}
                av_packet_rescale_ts(outPkt, encCtx->time_base, st->time_base);

                m_muxer->writePacket(outPkt);
                av_packet_unref(outPkt);
            }
        }
    }
    av_packet_free(&outPkt);
}

void StreamWorker::captureLoop() {
    while (m_captureRunning && !m_restartCapture) {
        AVFormatContext* inCtx = nullptr;
        AVCodecContext* decCtx = nullptr;

        QString currentUrl;
        { QMutexLocker locker(&m_urlMutex); currentUrl = m_url; }

        qDebug() << "Track" << m_trackIndex << "Attempting connection to:" << currentUrl;

        int videoStreamIdx = -1;
        if (!setupDecoder(&inCtx, &decCtx, currentUrl, &videoStreamIdx)) {
            qDebug() << "Track" << m_trackIndex << "Connect failed. Retrying in 1s...";
            // Sleep in small increments so we stay responsive to stop/restart requests
            for(int i=0; i<10 && m_captureRunning && !m_restartCapture; ++i)
                QThread::msleep(100);
            continue;
        }

        if (!m_sharedClock) {
            qDebug() << "Track" << m_trackIndex << "No shared clock. Restarting...";
            m_restartCapture = 1;
            m_captureRunning = false;
            break;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* rawFrame = av_frame_alloc();
        if (!pkt || !rawFrame) {
            qDebug() << "Track" << m_trackIndex << "Failed to allocate packet/frame.";
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
                            qf.frame->width = 1920;
                            qf.frame->height = 1080;
                            av_frame_get_buffer(qf.frame, 0);

                            // 3. Scale
                            m_swsCtx = sws_getCachedContext(m_swsCtx,
                                                            rawFrame->width, rawFrame->height, (AVPixelFormat)rawFrame->format,
                                                            1920, 1080, AV_PIX_FMT_YUV420P,
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
                if (m_lastPacketTimer.isValid() && m_lastPacketTimer.elapsed() > m_stallTimeoutMs) {
                    qDebug() << "Track" << m_trackIndex << "Stalled stream. Restarting...";
                    break;
                }
                QThread::msleep(10);
            } else if (readResult == AVERROR_EOF) {
                qDebug() << "Track" << m_trackIndex << "End of stream. Restarting...";
                break;
            } else {
                qDebug() << "Track" << m_trackIndex << "Read error (Disconnect).";
                break; // Break inner loop to trigger setupDecoder retry
            }
        }

        // CLEANUP INSIDE LOOP: Critical to prevent memory leaks on swap
        av_packet_free(&pkt);
        av_frame_free(&rawFrame);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inCtx);
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

    if (avformat_open_input(inCtx, currentUrl.toString().toUtf8().constData(), nullptr, &opts) < 0) {
        avformat_free_context(*inCtx);
        *inCtx = nullptr;
        if (opts) av_dict_free(&opts);
        return false;
    }

    // Keep blocking reads and rely on interrupt callback for stalls

    if (avformat_find_stream_info(*inCtx, nullptr) < 0) {
        avformat_close_input(inCtx);
        if (opts) av_dict_free(&opts);
        return false;
    }

    const AVCodec* decoder = nullptr;
    int foundStreamIdx = av_find_best_stream(*inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (foundStreamIdx < 0) {
        avformat_close_input(inCtx);
        if (opts) av_dict_free(&opts);
        return false;
    }

    *decCtx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(*decCtx, (*inCtx)->streams[foundStreamIdx]->codecpar);

    if (opts) av_dict_free(&opts);
    if (avcodec_open2(*decCtx, decoder, nullptr) < 0) {
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

    (*encCtx)->width = 1920;
    (*encCtx)->height = 1080;

    // --- CHANGE THIS: Use a standard MPEG-2 rate ---
    (*encCtx)->time_base = {1, 30};      // Internal codec clock (30fps)
    (*encCtx)->framerate = {30, 1};      // Target framerate
    // -----------------------------------------------

    (*encCtx)->pix_fmt = AV_PIX_FMT_YUV420P;
    (*encCtx)->gop_size = 1;             // Keep Intra-only for seeking
    (*encCtx)->bit_rate = 30000000;

    m_latestFrame = av_frame_alloc();
    if (!m_latestFrame) return false;
    m_latestFrame->format = AV_PIX_FMT_YUV420P;
    m_latestFrame->width = 1920;   // Your target width
    m_latestFrame->height = 1080;  // Your target height
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
    QMutexLocker locker(&m_urlMutex);
    m_url = newUrl;
    m_restartCapture = 1; // Signal the capture loop to break and restart
}
