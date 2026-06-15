#include "ffmpegingestsession.h"

#include <QDebug>
#include <QThread>
#include <QUrlQuery>

#include <cstring>
#include <thread>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/error.h>
    #include <libavutil/mem.h>
    #include <libavutil/samplefmt.h>
    #include <libswresample/swresample.h>
    #include <libswscale/swscale.h>
}

namespace {
constexpr int kAudioSampleRate = 48000;
}

FfmpegIngestSession::FfmpegIngestSession(int sourceIndex)
    : FfmpegIngestSession(sourceIndex, 1920, 1080, 30) {
}

FfmpegIngestSession::FfmpegIngestSession(int sourceIndex, int targetWidth, int targetHeight,
                                         int targetFps)
    : m_sourceIndex(sourceIndex) {
    if (targetWidth > 0) m_targetWidth = targetWidth;
    if (targetHeight > 0) m_targetHeight = targetHeight;
    if (targetFps > 0) m_targetFps = targetFps;
    m_monotonic.start();
}

FfmpegIngestSession::~FfmpegIngestSession() {
    requestStop();
    closeAsync();
}

bool FfmpegIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    closeAsync();
    m_callbacks = callbacks;
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_readingConnectedStream.store(false, std::memory_order_relaxed);
    m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
    return setupDecoder(url);
}

void FfmpegIngestSession::run() {
    if (!isOpen()) return;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* rawFrame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();
    if (!pkt || !rawFrame || !audioFrame) {
        log(QStringLiteral("Failed to allocate packet/frame."));
        if (pkt) av_packet_free(&pkt);
        if (rawFrame) av_frame_free(&rawFrame);
        if (audioFrame) av_frame_free(&audioFrame);
        closeAsync();
        return;
    }

    // Audio decoder and resampler setup.
    int swrInRate = 0;
    AVSampleFormat swrInFmt = AV_SAMPLE_FMT_NONE;
    AVChannelLayout swrInLayout;
    memset(&swrInLayout, 0, sizeof(swrInLayout));

    auto configureSwr = [&](int inRate, AVSampleFormat inFmt,
                            const AVChannelLayout* inLayout) -> bool {
        if (m_swrCtx) swr_free(&m_swrCtx);
        av_channel_layout_uninit(&swrInLayout);

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, 2);

        int ret = swr_alloc_set_opts2(&m_swrCtx, &outLayout, AV_SAMPLE_FMT_S16,
                                      kAudioSampleRate, inLayout, inFmt, inRate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
        if (ret < 0 || swr_init(m_swrCtx) < 0) {
            if (m_swrCtx) swr_free(&m_swrCtx);
            m_swrCtx = nullptr;
            log(QStringLiteral("Failed to init audio resampler"));
            return false;
        }
        swrInRate = inRate;
        swrInFmt = inFmt;
        av_channel_layout_copy(&swrInLayout, inLayout);
        qDebug() << "Source" << m_sourceIndex << "Audio: rate=" << inRate
                 << "ch=" << inLayout->nb_channels << "-> 48000 Hz stereo";
        return true;
    };

    const AVCodec* audioDecoder = nullptr;
    int foundAudioIdx = av_find_best_stream(m_inCtx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                            &audioDecoder, 0);
    if (foundAudioIdx >= 0 && audioDecoder) {
        m_audioDecCtx = avcodec_alloc_context3(audioDecoder);
        avcodec_parameters_to_context(m_audioDecCtx, m_inCtx->streams[foundAudioIdx]->codecpar);
        if (avcodec_open2(m_audioDecCtx, audioDecoder, nullptr) >= 0) {
            m_audioStreamIdx = foundAudioIdx;
            configureSwr(m_audioDecCtx->sample_rate, m_audioDecCtx->sample_fmt,
                         &m_audioDecCtx->ch_layout);
        } else {
            avcodec_free_context(&m_audioDecCtx);
            m_audioDecCtx = nullptr;
        }
    }

    int64_t anchorStreamTimeMs = -1;
    int64_t firstPacketDts = AV_NOPTS_VALUE;
    int64_t prevPktDts = AV_NOPTS_VALUE;

    int64_t firstAudioDts = AV_NOPTS_VALUE;
    int64_t audioAnchorStreamTimeMs = -1;
    int64_t prevAudioDts = AV_NOPTS_VALUE;

    m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
    m_readingConnectedStream.store(true, std::memory_order_relaxed);

    while (!shouldStop()) {
        int readResult = av_read_frame(m_inCtx, pkt);
        if (readResult >= 0) {
            m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
            if (pkt->stream_index == m_videoStreamIdx) {
                int64_t pktDts = pkt->dts;
                if (pktDts == AV_NOPTS_VALUE) pktDts = pkt->pts;
                if (pktDts == AV_NOPTS_VALUE) {
                    av_packet_unref(pkt);
                    continue;
                }

                const AVRational videoTb = m_inCtx->streams[m_videoStreamIdx]->time_base;

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
                    anchorStreamTimeMs = m_callbacks.recordingClockMs
                                             ? m_callbacks.recordingClockMs()
                                             : -1;
                }
                prevPktDts = pktDts;

                if (avcodec_send_packet(m_decCtx, pkt) >= 0) {
                    while (avcodec_receive_frame(m_decCtx, rawFrame) >= 0) {
                        AVFrame* scaledFrame = av_frame_alloc();
                        if (!scaledFrame) {
                            av_frame_unref(rawFrame);
                            continue;
                        }
                        scaledFrame->format = AV_PIX_FMT_YUV420P;
                        scaledFrame->width = m_targetWidth;
                        scaledFrame->height = m_targetHeight;
                        if (av_frame_get_buffer(scaledFrame, 0) < 0) {
                            av_frame_free(&scaledFrame);
                            av_frame_unref(rawFrame);
                            continue;
                        }

                        m_swsCtx = sws_getCachedContext(
                            m_swsCtx, rawFrame->width, rawFrame->height,
                            static_cast<AVPixelFormat>(rawFrame->format), m_targetWidth,
                            m_targetHeight, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr,
                            nullptr);
                        if (!m_swsCtx) {
                            av_frame_free(&scaledFrame);
                            av_frame_unref(rawFrame);
                            continue;
                        }

                        sws_scale(m_swsCtx, rawFrame->data, rawFrame->linesize, 0,
                                  rawFrame->height, scaledFrame->data, scaledFrame->linesize);

                        int64_t frameTs = rawFrame->best_effort_timestamp;
                        if (frameTs == AV_NOPTS_VALUE) frameTs = pktDts;
                        int64_t relativeMs =
                            av_rescale_q(frameTs - firstPacketDts, videoTb, {1, 1000});

                        DecodedVideoFrame decoded;
                        decoded.frame = scaledFrame;
                        decoded.sourcePtsMs = anchorStreamTimeMs + relativeMs;
                        if (m_callbacks.onVideoFrame) {
                            m_callbacks.onVideoFrame(decoded);
                        } else {
                            av_frame_free(&scaledFrame);
                        }

                        av_frame_unref(rawFrame);
                    }
                }
            } else if (pkt->stream_index == m_audioStreamIdx && m_audioDecCtx) {
                if (avcodec_send_packet(m_audioDecCtx, pkt) >= 0) {
                    while (avcodec_receive_frame(m_audioDecCtx, audioFrame) >= 0) {
                        int64_t audioTs = audioFrame->pts;
                        if (audioTs == AV_NOPTS_VALUE)
                            audioTs = audioFrame->best_effort_timestamp;
                        if (audioTs == AV_NOPTS_VALUE) {
                            av_frame_unref(audioFrame);
                            continue;
                        }
                        const AVRational audioTb =
                            m_inCtx->streams[m_audioStreamIdx]->time_base;

                        const int inRate = audioFrame->sample_rate > 0
                                               ? audioFrame->sample_rate
                                               : m_audioDecCtx->sample_rate;
                        const AVSampleFormat inFmt =
                            static_cast<AVSampleFormat>(audioFrame->format);
                        if (!m_swrCtx || inRate != swrInRate || inFmt != swrInFmt ||
                            av_channel_layout_compare(&audioFrame->ch_layout, &swrInLayout) != 0) {
                            if (m_swrCtx)
                                qDebug()
                                    << "Source" << m_sourceIndex
                                    << "Audio format change mid-stream; rebuilding resampler";
                            configureSwr(inRate, inFmt, &audioFrame->ch_layout);
                        }
                        if (!m_swrCtx) {
                            av_frame_unref(audioFrame);
                            continue;
                        }

                        const int64_t nowMs = m_callbacks.recordingClockMs
                                                  ? m_callbacks.recordingClockMs()
                                                  : -1;
                        bool needAudioAnchor = (firstAudioDts == AV_NOPTS_VALUE);
                        if (!needAudioAnchor && prevAudioDts != AV_NOPTS_VALUE) {
                            const int64_t aDeltaMs =
                                av_rescale_q(audioTs - prevAudioDts, audioTb, {1, 1000});
                            constexpr int64_t kForwardJumpMs = 3000;
                            constexpr int64_t kBackwardTolMs = -200;
                            if (aDeltaMs > kForwardJumpMs || aDeltaMs < kBackwardTolMs) {
                                qDebug() << "Source" << m_sourceIndex
                                         << "Audio DTS discontinuity (" << aDeltaMs
                                         << "ms jump). Re-anchoring.";
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

                        if (recPtsMs > nowMs + 10000) {
                            qDebug() << "Source" << m_sourceIndex << "Audio sample" << recPtsMs
                                     << "ms far ahead of clock" << nowMs << "ms. Re-anchoring.";
                            firstAudioDts = audioTs;
                            audioAnchorStreamTimeMs = nowMs;
                            prevAudioDts = audioTs;
                            recPtsMs = nowMs;
                        }
                        if (recPtsMs < 0) {
                            av_frame_unref(audioFrame);
                            continue;
                        }

                        int outSamples = av_rescale_rnd(
                            swr_get_delay(m_swrCtx, inRate) + audioFrame->nb_samples,
                            kAudioSampleRate, inRate, AV_ROUND_UP);
                        int outBufSize = av_samples_get_buffer_size(
                            nullptr, 2, outSamples, AV_SAMPLE_FMT_S16, 0);
                        uint8_t* outBuffer = static_cast<uint8_t*>(av_malloc(outBufSize));
                        if (!outBuffer) {
                            av_frame_unref(audioFrame);
                            continue;
                        }
                        uint8_t* outBuf[1] = { outBuffer };

                        int converted = swr_convert(
                            m_swrCtx, outBuf, outSamples,
                            const_cast<const uint8_t**>(audioFrame->extended_data),
                            audioFrame->nb_samples);

                        if (converted > 0 && m_callbacks.onAudioChunk) {
                            DecodedAudioChunk chunk;
                            chunk.startSample = recPtsMs * kAudioSampleRate / 1000;
                            chunk.pcmS16Stereo = QByteArray(
                                reinterpret_cast<const char*>(outBuffer),
                                converted * kDecodedAudioBytesPerSample);
                            m_callbacks.onAudioChunk(std::move(chunk));
                        }

                        av_freep(&outBuffer);
                        av_frame_unref(audioFrame);
                    }
                }
            }
            av_packet_unref(pkt);
        } else if (readResult == AVERROR(EAGAIN)) {
            const int64_t lastPkt = m_lastPacketAtMs.load(std::memory_order_relaxed);
            if (m_readingConnectedStream.load(std::memory_order_relaxed) && lastPkt >= 0
                && m_monotonic.elapsed() - lastPkt > m_stallTimeoutMs) {
                log(QStringLiteral("Stalled stream. Restarting..."));
                break;
            }
            QThread::msleep(10);
        } else if (readResult == AVERROR(ETIMEDOUT) || readResult == AVERROR_EXIT) {
            log(QStringLiteral("Timeout/Exit. Restarting..."));
            break;
        } else if (readResult == AVERROR_EOF) {
            log(QStringLiteral("End of stream. Restarting..."));
            break;
        } else {
            log(QStringLiteral("Read error (Disconnect)."));
            break;
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&rawFrame);
    av_frame_free(&audioFrame);

    if (m_swrCtx) {
        int flushed = 1;
        while (flushed > 0) {
            uint8_t* flushBuf = static_cast<uint8_t*>(av_malloc(4096));
            if (!flushBuf) break;
            uint8_t* flushBufs[1] = { flushBuf };
            flushed = swr_convert(m_swrCtx, flushBufs, 1024, nullptr, 0);
            if (flushed > 0 && m_callbacks.onAudioChunk) {
                DecodedAudioChunk chunk;
                chunk.startSample = -1;
                chunk.pcmS16Stereo = QByteArray(reinterpret_cast<const char*>(flushBuf),
                                                flushed * kDecodedAudioBytesPerSample);
                m_callbacks.onAudioChunk(std::move(chunk));
            }
            av_freep(&flushBuf);
        }
    }

    av_channel_layout_uninit(&swrInLayout);
    closeAsync();
}

void FfmpegIngestSession::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
}

bool FfmpegIngestSession::isOpen() const {
    return m_open.load(std::memory_order_relaxed);
}

int FfmpegIngestSession::ffmpegInterruptCallback(void* opaque) {
    FfmpegIngestSession* session = static_cast<FfmpegIngestSession*>(opaque);
    return session ? session->shouldStop() : 0;
}

bool FfmpegIngestSession::setupDecoder(const QUrl& url) {
    AVDictionary* opts = nullptr;
    QUrl currentUrl = url;
    const QString scheme = currentUrl.scheme().toLower();

    av_dict_set(&opts, "rw_timeout", "5000000", 0);
    av_dict_set(&opts, "timeout", "5000000", 0);
    av_dict_set(&opts, "recv_buffer_size", "15048000", 0);

    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "probesize", "500000", 0);
    av_dict_set(&opts, "analyzeduration", "500000", 0);

    if (scheme == "srt") {
        av_dict_set(&opts, "connect_timeout", "5000000", 0);
        av_dict_set(&opts, "latency", "500", 0);
        av_dict_set(&opts, "rcvlatency", "500", 0);
        av_dict_set(&opts, "peerlatency", "500", 0);
        av_dict_set(&opts, "transtype", "live", 0);
        // SRT-private options set on the avformat_open_input() dict do not all
        // propagate to the nested SRT URLContext. Put linger in the URL query
        // so close drops immediately instead of blocking on SRT's 180 s default.
        QUrlQuery srtQuery(currentUrl);
        if (!srtQuery.hasQueryItem(QStringLiteral("linger")))
            srtQuery.addQueryItem(QStringLiteral("linger"), QStringLiteral("0"));
        currentUrl.setQuery(srtQuery);
    }

    if (scheme == "rtmp" || scheme == "rtmps") {
        av_dict_set(&opts, "rtmp_buffer", "5000", 0);
        av_dict_set(&opts, "rtmp_live", "live", 0);
    }

    m_inCtx = avformat_alloc_context();
    if (!m_inCtx) {
        if (opts) av_dict_free(&opts);
        return false;
    }

    m_lastPacketAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);

    m_inCtx->interrupt_callback.callback = &FfmpegIngestSession::ffmpegInterruptCallback;
    m_inCtx->interrupt_callback.opaque = this;

    int openResult = avformat_open_input(&m_inCtx,
                                         currentUrl.toString().toUtf8().constData(),
                                         nullptr,
                                         &opts);
    if (openResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(openResult, errbuf, sizeof(errbuf));
        qDebug() << "Source" << m_sourceIndex << "avformat_open_input failed:" << errbuf;
        avformat_free_context(m_inCtx);
        m_inCtx = nullptr;
        if (opts) av_dict_free(&opts);
        return false;
    }

    int infoResult = avformat_find_stream_info(m_inCtx, nullptr);
    if (infoResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(infoResult, errbuf, sizeof(errbuf));
        qDebug() << "Source" << m_sourceIndex << "avformat_find_stream_info failed:" << errbuf;
        avformat_close_input(&m_inCtx);
        if (opts) av_dict_free(&opts);
        return false;
    }

    const AVCodec* decoder = nullptr;
    int foundStreamIdx = av_find_best_stream(m_inCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (foundStreamIdx < 0) {
        log(QStringLiteral("No video stream found."));
        avformat_close_input(&m_inCtx);
        if (opts) av_dict_free(&opts);
        return false;
    }

    m_decCtx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(m_decCtx, m_inCtx->streams[foundStreamIdx]->codecpar);

    if (opts) av_dict_free(&opts);
    int codecResult = avcodec_open2(m_decCtx, decoder, nullptr);
    if (codecResult < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(codecResult, errbuf, sizeof(errbuf));
        qDebug() << "Source" << m_sourceIndex << "avcodec_open2 failed:" << errbuf;
        avcodec_free_context(&m_decCtx);
        avformat_close_input(&m_inCtx);
        return false;
    }

    m_videoStreamIdx = foundStreamIdx;
    m_open.store(true, std::memory_order_relaxed);
    return true;
}

void FfmpegIngestSession::closeAsync() {
    m_readingConnectedStream.store(false, std::memory_order_relaxed);
    m_open.store(false, std::memory_order_relaxed);

    AVCodecContext* decToFree = m_decCtx;
    AVCodecContext* audioDecToFree = m_audioDecCtx;
    SwrContext* swrToFree = m_swrCtx;
    SwsContext* swsToFree = m_swsCtx;
    AVFormatContext* fmtToClose = m_inCtx;
    m_decCtx = nullptr;
    m_audioDecCtx = nullptr;
    m_swrCtx = nullptr;
    m_swsCtx = nullptr;
    m_inCtx = nullptr;
    m_videoStreamIdx = -1;
    m_audioStreamIdx = -1;

    if (fmtToClose) {
        fmtToClose->interrupt_callback.callback = [](void*) -> int { return 1; };
        fmtToClose->interrupt_callback.opaque = nullptr;
    }

    if (!decToFree && !audioDecToFree && !swrToFree && !swsToFree && !fmtToClose) return;

    std::thread([decToFree, audioDecToFree, swrToFree, swsToFree, fmtToClose]() mutable {
        if (decToFree) avcodec_free_context(&decToFree);
        if (audioDecToFree) avcodec_free_context(&audioDecToFree);
        if (swrToFree) swr_free(&swrToFree);
        if (swsToFree) sws_freeContext(swsToFree);
        if (fmtToClose) avformat_close_input(&fmtToClose);
    }).detach();
}

bool FfmpegIngestSession::shouldStop() const {
    if (m_stopRequested.load(std::memory_order_relaxed)) return true;
    if (m_callbacks.shouldStop && m_callbacks.shouldStop()) return true;
    const int64_t lastPkt = m_lastPacketAtMs.load(std::memory_order_relaxed);
    if (m_readingConnectedStream.load(std::memory_order_relaxed) && lastPkt >= 0
        && m_monotonic.elapsed() - lastPkt > m_stallTimeoutMs) {
        return true;
    }
    return false;
}

void FfmpegIngestSession::log(const QString& message) const {
    if (m_callbacks.logInfo) {
        m_callbacks.logInfo(message);
    } else {
        qDebug() << "Source" << m_sourceIndex << message;
    }
}
