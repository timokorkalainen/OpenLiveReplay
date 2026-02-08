#include "playback/playbackworker.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>

PlaybackWorker::PlaybackWorker(const QList<FrameProvider*> &providers, PlaybackTransport *transport, QObject *parent)
    : QThread(parent)
{
    m_transport = transport;
    m_providers = providers;
}

PlaybackWorker::~PlaybackWorker() {
    stop();
    for (auto* track : m_decoderBank) {
        if (track->codecCtx) avcodec_free_context(&track->codecCtx);
        delete track;
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

void PlaybackWorker::stop() {
    m_running = false;
    wait();
}

void PlaybackWorker::run() {
    msleep(500);
    m_transport->seek(0);

    qDebug() << "Opening file: "<<m_currentFilePath;

    if (m_currentFilePath.isEmpty()) return;

    m_running = true;

    auto clearDecoders = [this]() {
        QMutexLocker bufferLocker(&m_bufferMutex);
        for (auto* track : m_decoderBank) {
            if (track->codecCtx) avcodec_free_context(&track->codecCtx);
            delete track;
        }
        m_decoderBank.clear();
        m_streamMap.clear();
    };

    // --- 1. OPENING & INITIALIZATION (retry until tracks available or stop) ---
    while (m_running) {
        if (m_fmtCtx) {
            avformat_close_input(&m_fmtCtx);
        }
        clearDecoders();

        if (avformat_open_input(&m_fmtCtx, m_currentFilePath.toUtf8().constData(), nullptr, nullptr) < 0) {
            msleep(200);
            continue;
        }
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

        if (!m_decoderBank.isEmpty()) break;

        qDebug() << "PlaybackWorker: No video tracks yet. Retrying...";
        avformat_close_input(&m_fmtCtx);
        msleep(500);
    }

    if (!m_running || m_decoderBank.isEmpty()) {
        clearDecoders();
        if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
        return;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    int64_t lastProcessedPtsMs = -1;
    m_seekTargetMs = -1;

    while (m_running) {
        // --- 2. GET MASTER TIME FROM TRANSPORT ---
        int64_t masterTimeMs = m_transport->currentPos();

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
                int burstCount = 0;
                int packetCount = 0;
                const int bufMax = m_frameBufferMax;
                const int packetMax = bufMax * 4; // Safety limit for non-video packets
                while (m_running && burstCount < bufMax && packetCount < packetMax) {
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
                                        track->buffer.append({framePtsMs, qFrame});
                                        if (track->buffer.size() > m_frameBufferMax) {
                                            track->buffer.remove(0, track->buffer.size() - m_frameBufferMax);
                                        }
                                    }
                                    burstCount++;
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

            continue;
        }

        m_mutex.unlock();

        // --- 4. PACE CONTROL (SYNC) ---
        // If our last frame is in the future compared to the Master Clock, wait.
        if (lastProcessedPtsMs > masterTimeMs) {
            msleep(5); // Smallest sleep to remain responsive to transport changes
            continue;
        }

        // --- 5. READ & DECODE ---
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
                                track->buffer.append({framePtsMs, qFrame});
                                if (track->buffer.size() > m_frameBufferMax) {
                                    track->buffer.remove(0, track->buffer.size() - m_frameBufferMax);
                                }
                            }

                            track->provider->deliverFrame(qFrame);
                        }
                    }
                    break;
                }
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
    }

    // --- 6. CLEANUP ---
    av_packet_free(&pkt);
    av_frame_free(&frame);
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
            if (track->buffer.isEmpty()) continue;
            for (int i = track->buffer.size() - 1; i >= 0; --i) {
                if (track->buffer[i].ptsMs <= targetMs) {
                    pending.append({track->provider, track->buffer[i].frame});
                    break;
                }
            }
        }
    }

    if (pending.isEmpty()) return false;

    for (const auto &item : pending) {
        if (item.provider) item.provider->deliverFrame(item.frame);
    }
    return true;
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
