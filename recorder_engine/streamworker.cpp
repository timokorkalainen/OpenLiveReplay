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

    if (m_restartCapture || !m_captureRunning) {
        // If the future is valid and running, wait for it to cleanly exit
        // before starting a new source
        if (!m_captureFuture.isFinished() && m_captureFuture.isValid()) {
            m_captureRunning = false;
            m_captureFuture.waitForFinished();
        }

        m_restartCapture = 0;
        m_captureRunning = true;

        // Assign the new future here
        m_captureFuture = QtConcurrent::run([this]() {
            this->captureLoop();
        });
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

        if (!setupDecoder(&inCtx, &decCtx, currentUrl)) {
            qDebug() << "Track" << m_trackIndex << "Connect failed. Retrying in 1s...";
            // Sleep in small increments so we stay responsive to stop/restart requests
            for(int i=0; i<10 && m_captureRunning && !m_restartCapture; ++i)
                QThread::msleep(100);
            continue;
        }

        if (!m_sharedClock) return;

        AVPacket* pkt = av_packet_alloc();
        AVFrame* rawFrame = av_frame_alloc();

        const AVCodec* decoder = nullptr;
        int videoStreamIdx = av_find_best_stream(inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);

        int firstSourcePts = -1;

        int64_t anchorStreamTimeMs = -1;
        int64_t firstPacketDts = -1;

        while (m_captureRunning && !m_restartCapture) {
            if (av_read_frame(inCtx, pkt) >= 0) {
                if (pkt->stream_index == videoStreamIdx) {

                    // 1. Establish the Anchor on the very first packet of this URL session
                    if (firstPacketDts == -1) {
                        firstPacketDts = pkt->dts;
                        // Where are we in the global recording right now?
                        anchorStreamTimeMs = m_sharedClock->elapsedMs();
                    }

                    if (avcodec_send_packet(decCtx, pkt) >= 0) {
                        while (avcodec_receive_frame(decCtx, rawFrame) >= 0) {

                            // 1. Initialize start time correctly using the packet DTS
                            if (firstSourcePts == -1) firstSourcePts = pkt->dts;

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
                            int64_t relativeMs = av_rescale_q(pkt->dts - firstPacketDts,
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
            }else {
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

bool StreamWorker::setupDecoder(AVFormatContext** inCtx, AVCodecContext** decCtx, QUrl currentUrl) {
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "stimeout", "100000", 0); // 100ms timeout for network

    if (avformat_open_input(inCtx, currentUrl.toString().toUtf8().constData(), nullptr, &opts) < 0) return false;

    // Set the context to non-blocking mode
    (*inCtx)->flags |= AVFMT_FLAG_NONBLOCK;

    if (avformat_find_stream_info(*inCtx, nullptr) < 0) return false;

    const AVCodec* decoder = nullptr;
    int videoStreamIdx = av_find_best_stream(*inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (videoStreamIdx < 0) return false;

    *decCtx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(*decCtx, (*inCtx)->streams[videoStreamIdx]->codecpar);
    return avcodec_open2(*decCtx, decoder, nullptr) >= 0;
}

bool StreamWorker::setupEncoder(AVCodecContext** encCtx) {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    *encCtx = avcodec_alloc_context3(encoder);

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
    m_latestFrame->format = AV_PIX_FMT_YUV420P;
    m_latestFrame->width = 1920;   // Your target width
    m_latestFrame->height = 1080;  // Your target height
    av_frame_get_buffer(m_latestFrame, 0); // Allocate actual pixel memory

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
