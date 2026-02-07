#include "playback/playbackworker.h"
#include <QDebug>
#include <QElapsedTimer>

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

void PlaybackWorker::stop() {
    m_running = false;
    wait();
}

void PlaybackWorker::run() {
    msleep(500);
    m_transport->seek(0);

    qDebug() << "Opening file: "<<m_currentFilePath;

    if (m_currentFilePath.isEmpty()) return;

    // --- 1. OPENING & INITIALIZATION ---
    if (avformat_open_input(&m_fmtCtx, m_currentFilePath.toUtf8().constData(), nullptr, nullptr) < 0) return;
    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) return;

    // --- INITIALIZE DECODER BANK ---
    int providerIndex = 0;
    for (unsigned int i = 0; i < m_fmtCtx->nb_streams; i++) {
        AVStream* stream = m_fmtCtx->streams[i];
        AVCodecParameters* codecParams = stream->codecpar;

        if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO) {
            // Safety: Don't exceed the number of providers we have in the UI
            if (providerIndex >= m_providers.size()) break;

            const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(ctx, codecParams);

            // Enable Multi-threading for the decoder itself
            ctx->thread_count = 0;

            if (avcodec_open2(ctx, codec, nullptr) < 0) continue;

            DecoderTrack* track = new DecoderTrack();
            track->streamIndex = i;
            track->codecCtx = ctx;
            track->provider = m_providers[providerIndex];

            m_decoderBank.append(track);
            m_streamMap.insert(i, track);

            providerIndex++;
            qDebug() << "Worker: Initialized Decoder for Stream" << i << "mapped to Provider" << (providerIndex-1);
        }
    }

    m_running = true;
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
        if (m_seekTargetMs >= 0 || qAbs(masterTimeMs - lastProcessedPtsMs) > 500) {
            int64_t target = (m_seekTargetMs >= 0) ? m_seekTargetMs : masterTimeMs;

            // Map MS to stream timebase (using the first video track as reference)
            AVStream* vStream = m_fmtCtx->streams[m_decoderBank[0]->streamIndex];
            int64_t seekPts = av_rescale_q(target, {1, 1000}, vStream->time_base);

            av_seek_frame(m_fmtCtx, vStream->index, seekPts, AVSEEK_FLAG_BACKWARD);

            // Flush all decoders in the bank
            for(auto* track : m_decoderBank) {
                if(track->codecCtx) avcodec_flush_buffers(track->codecCtx);
            }

            lastProcessedPtsMs = target;
            m_seekTargetMs = -1;
            m_mutex.unlock();
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

                            // Convert frame PTS to Milliseconds
                            AVRational tb = m_fmtCtx->streams[track->streamIndex]->time_base;
                            lastProcessedPtsMs = av_rescale_q(frame->pts, tb, {1, 1000});

                            track->provider->deliverFrame(convertToQVideoFrame(frame));
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
