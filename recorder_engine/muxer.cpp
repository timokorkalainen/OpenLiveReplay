#include "muxer.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

Muxer::Muxer() {}

Muxer::~Muxer() { close(); }

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
                 int audioSampleRate, int audioChannels,
                 VideoCodecChoice codec, const QByteArray& videoExtradata) {
    return init(filename, videoTrackCount, width, height, fps, streamNames, {}, {}, audioSampleRate, audioChannels,
                codec, videoExtradata);
}

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
                 const QStringList& telemetryFeedIds, const QStringList& telemetryFeedNames,
                 int audioSampleRate, int audioChannels,
                 VideoCodecChoice codec, const QByteArray& videoExtradata) {
    QMutexLocker locker(&m_mutex);
    Q_UNUSED(streamNames);

    m_telemetryTrackOffset = 0;
    m_telemetryTrackCount = 0;
    const auto resetTelemetryTracks = [this] {
        m_telemetryTrackOffset = 0;
        m_telemetryTrackCount = 0;
    };

    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;
    if (fps <= 0) fps = 30;

    // 1. Create Format Context for Matroska
    m_activePath = getVideoPath(filename);
    avformat_alloc_output_context2(&m_outCtx, nullptr, "matroska", m_activePath.toUtf8().constData());
    if (!m_outCtx) return false;

    if (codec == VideoCodecChoice::H264Hardware && videoExtradata.isEmpty()) {
        qWarning() << "Muxer: H.264 selected but no avcC extradata provided; refusing to init.";
        avformat_free_context(m_outCtx);
        m_outCtx = nullptr;
        resetTelemetryTracks();
        return false;
    }

    // 2. Pre-allocate Video Tracks
    for (int i = 0; i < videoTrackCount; i++) {
        AVStream* st = avformat_new_stream(m_outCtx, nullptr);
        st->id = i;

        // 1. Set parameters
        st->codecpar->codec_id = (codec == VideoCodecChoice::H264Hardware)
                                     ? AV_CODEC_ID_H264
                                     : AV_CODEC_ID_MPEG2VIDEO;
        if (codec == VideoCodecChoice::H264Hardware) {
            // Invariant: videoExtradata is guaranteed non-empty by the up-front
            // guard at the top of init() (which returns false for H264Hardware
            // with empty extradata), so no emptiness re-check is needed here.
            // H.264 requires avcC (AVCDecoderConfigurationRecord) as CodecPrivate
            // in Matroska; MPEG-2 does not attach extradata and omits this block.
            st->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(videoExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!st->codecpar->extradata) {
                qWarning() << "Muxer: failed to allocate H.264 extradata.";
                avformat_free_context(m_outCtx);
                m_outCtx = nullptr;
                resetTelemetryTracks();
                return false;
            }
            memcpy(st->codecpar->extradata, videoExtradata.constData(), videoExtradata.size());
            st->codecpar->extradata_size = videoExtradata.size();
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->width = width;
        st->codecpar->height = height;
        st->codecpar->format = AV_PIX_FMT_YUV420P;
        st->codecpar->bit_rate = 30000000;

        // 2. Use millisecond timebase for Matroska
        // This keeps timeline duration consistent across players.
        st->time_base = {1, 1000};

        // 3. Set the metadata hints
        st->avg_frame_rate = {fps, 1};
        st->r_frame_rate = {fps, 1};

        // For MPEG-2, you can also set the 'closed gop' and 'fixed fps' flags in codecpar
        st->codecpar->video_delay = 0;

        // Always name video tracks generically: "Track 1", "Track 2", ...
        const QString trackTitle = QString("Track %1").arg(i + 1);
        av_dict_set(&st->metadata, "title", trackTitle.toUtf8().constData(), 0);
    }

    // 2a. Add one audio track per video track (PCM S16LE, 48 kHz stereo)
    m_audioTrackOffset = videoTrackCount;
    for (int i = 0; i < videoTrackCount; i++) {
        AVStream* st = avformat_new_stream(m_outCtx, nullptr);
        st->id = videoTrackCount + i;
        st->codecpar->codec_id    = AV_CODEC_ID_PCM_S16LE;
        st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        st->codecpar->sample_rate = audioSampleRate;
        st->codecpar->format      = AV_SAMPLE_FMT_S16;
        st->codecpar->bit_rate    = audioSampleRate * audioChannels * 16;
        st->codecpar->frame_size  = 0; // PCM has no fixed frame size
        av_channel_layout_default(&st->codecpar->ch_layout, audioChannels);
        st->time_base = {1, 1000};

        const QString audioTitle = QString("Track %1 Audio").arg(i + 1);
        av_dict_set(&st->metadata, "title", audioTitle.toUtf8().constData(), 0);
    }

    // 2b. Add one subtitle track per video track for per-frame source metadata
    m_subtitleTrackOffset = videoTrackCount * 2;
    for (int i = 0; i < videoTrackCount; i++) {
        AVStream* st = avformat_new_stream(m_outCtx, nullptr);
        st->id = m_subtitleTrackOffset + i;
        st->codecpar->codec_id    = AV_CODEC_ID_TEXT;
        st->codecpar->codec_type  = AVMEDIA_TYPE_SUBTITLE;
        st->time_base = {1, 1000};

        const QString subTitle = QString("Track %1 Metadata").arg(i + 1);
        av_dict_set(&st->metadata, "title", subTitle.toUtf8().constData(), 0);
    }

    // 2c. Add one subtitle track per configured feed for feed telemetry
    m_telemetryTrackOffset = m_subtitleTrackOffset + videoTrackCount;
    m_telemetryTrackCount = telemetryFeedIds.size();
    for (int i = 0; i < telemetryFeedIds.size(); ++i) {
        AVStream* st = avformat_new_stream(m_outCtx, nullptr);
        st->id = m_telemetryTrackOffset + i;
        st->codecpar->codec_id = AV_CODEC_ID_TEXT;
        st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        st->time_base = {1, 1000};

        const QString feedId = telemetryFeedIds.at(i);
        const QString feedName = i < telemetryFeedNames.size() ? telemetryFeedNames.at(i) : QString();
        const QString title = QString("Feed %1 Telemetry").arg(feedId);
        av_dict_set(&st->metadata, "title", title.toUtf8().constData(), 0);
        av_dict_set(&st->metadata, "olr_track_type", "feed_telemetry", 0);
        av_dict_set(&st->metadata, "olr_feed_id", feedId.toUtf8().constData(), 0);
        av_dict_set(&st->metadata, "olr_feed_name", feedName.toUtf8().constData(), 0);
    }

    // 3. Set Matroska specific options for Chase Play
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "reserve_index_space", "1024k", 0); // Crucial for seeking while recording
    av_dict_set(&opts, "cluster_size_limit", "1M", 0);      // Flush data often
    av_dict_set(&opts, "cluster_time_limit", "100", 0); // Flush data to disk every 100ms
    av_dict_set(&opts, "live", "1", 0);                 // Signal this is a live-streamed file

    // 3b. Store recording start time metadata (UTC ISO-8601)
    const QString startIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    av_dict_set(&m_outCtx->metadata, "recording_start_time", startIso.toUtf8().constData(), 0);

    // 4. Open file and write header
    if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_outCtx->pb, m_outCtx->url, AVIO_FLAG_WRITE) < 0) {
            av_dict_free(&opts);
            avformat_free_context(m_outCtx);
            m_outCtx = nullptr;
            resetTelemetryTracks();
            return false;
        }
    }

    const int headerRet = avformat_write_header(m_outCtx, &opts);
    av_dict_free(&opts);
    if (headerRet < 0) {
        if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_outCtx->pb);
        }
        avformat_free_context(m_outCtx);
        m_outCtx = nullptr;
        resetTelemetryTracks();
        return false;
    }
    avio_flush(m_outCtx->pb); // Forces the EBML header to be visible to the reader

    m_lastDts.clear();
    m_lastFlush.start();

    m_initialized = true;

    // Start the dedicated writer thread now that the header is written and
    // m_outCtx is fully ready. From here until close() joins it, the writer
    // thread is the ONLY thread that touches m_outCtx (av_write_frame/flush).
    m_writerRunning = true;
    m_writerThread = std::thread(&Muxer::writerLoop, this);

    return true;
}

void Muxer::writePacket(AVPacket* pkt) {
    // ENQUEUE-ONLY. Clone the caller's packet (the caller still owns theirs,
    // exactly as before) and hand the clone to the writer thread, then return
    // immediately. The DTS-bump, av_write_frame and avio_flush all happen on
    // the writer thread — so a stalled disk no longer blocks the caller.
    if (!m_writerRunning.load(std::memory_order_acquire)) return;

    AVPacket* localPkt = av_packet_clone(pkt);
    if (!localPkt) return;

    std::unique_lock<std::mutex> lk(m_qMutex);
    // Backpressure: never drop (dropping corrupts the file) and never grow
    // unbounded. A transient stall is absorbed by the queue; a SUSTAINED
    // disk-too-slow eventually blocks the caller here — unavoidable, the disk
    // literally cannot keep up — but it is still strictly better than blocking
    // on every single packet. Re-check m_writerRunning so close() can wake us.
    m_qCv.wait(lk, [this] {
        return m_pktQueue.size() < kMaxQueued || !m_writerRunning.load(std::memory_order_acquire);
    });
    if (!m_writerRunning.load(std::memory_order_acquire)) {
        // Shutting down; do not enqueue (close() is draining/finishing).
        av_packet_free(&localPkt);
        return;
    }
    m_pktQueue.push(localPkt);
    lk.unlock();
    m_qCv.notify_one();
}

void Muxer::writerLoop() {
    for (;;) {
        AVPacket* pkt = nullptr;
        {
            std::unique_lock<std::mutex> lk(m_qMutex);
            // Wait for work, or for shutdown. Keep draining while the queue is
            // non-empty even after running==false, so every queued packet is
            // written before we exit (close() relies on this for the trailer).
            m_qCv.wait(lk, [this] {
                return !m_pktQueue.empty() || !m_writerRunning.load(std::memory_order_acquire);
            });
            if (m_pktQueue.empty()) {
                // Queue drained AND shutdown requested → done.
                if (!m_writerRunning.load(std::memory_order_acquire)) return;
                continue;
            }
            pkt = m_pktQueue.front();
            m_pktQueue.pop();
        }
        // Notify a possibly back-pressured producer that there is now room.
        m_qCv.notify_one();

        // ── No q lock held below; only this thread touches m_outCtx/m_lastDts.

        // Ensure monotonic DTS per stream — bump forward if needed.
        // Dropping was too aggressive: when a source was re-mapped to a view
        // track that had blue-frame DTS ahead of the source encoder's counter,
        // every packet was silently lost.  Bumping preserves the data.
        const int idx = pkt->stream_index;
        auto it = m_lastDts.constFind(idx);
        if (it != m_lastDts.constEnd() && pkt->dts <= it.value()) {
            pkt->dts = it.value() + 1;
            if (pkt->pts < pkt->dts) pkt->pts = pkt->dts;
        }
        m_lastDts[idx] = pkt->dts;

        // Use av_write_frame (non-interleaved) so that each stream writes
        // independently. av_interleaved_write_frame buffers packets across
        // ALL streams and won't flush stream A until stream B catches up,
        // causing one disrupted source to freeze every other source.
        const int ret = av_write_frame(m_outCtx, pkt);
        av_packet_free(&pkt); // av_write_frame does NOT take ownership

        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            qDebug() << "Muxer: write error for stream" << idx << ":" << errbuf;
        }

        // Flush at most every ~100 ms: keeps the chase-play reader within a
        // cluster of the live edge without a disk flush per packet.
        if (!m_lastFlush.isValid() || m_lastFlush.elapsed() >= 100) {
            avio_flush(m_outCtx->pb);
            m_lastFlush.restart();
        }
    }
}

void Muxer::writeMetadataPacket(int viewTrack, int64_t ptsMs, const QByteArray& jsonData) {
    if (!m_initialized || !m_outCtx || jsonData.isEmpty()) return;

    const int subTrackIndex = m_subtitleTrackOffset + viewTrack;
    if (subTrackIndex < 0 || subTrackIndex >= (int)m_outCtx->nb_streams) return;

    AVStream* st = m_outCtx->streams[subTrackIndex];
    if (!st) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    if (av_new_packet(pkt, jsonData.size()) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, jsonData.constData(), jsonData.size());

    pkt->stream_index = subTrackIndex;
    pkt->pts      = av_rescale_q(ptsMs, {1, 1000}, st->time_base);
    pkt->dts      = pkt->pts;
    pkt->duration = av_rescale_q(1, {1, 1000}, st->time_base);

    writePacket(pkt);
    av_packet_free(&pkt);
}

void Muxer::writeTelemetryPacket(int feedIndex, int64_t ptsMs, const QByteArray& jsonData) {
    if (!m_initialized || !m_outCtx || jsonData.isEmpty()) return;
    if (feedIndex < 0 || feedIndex >= m_telemetryTrackCount) return;

    const int trackIndex = m_telemetryTrackOffset + feedIndex;
    if (trackIndex < 0 || trackIndex >= (int)m_outCtx->nb_streams) return;

    AVStream* st = m_outCtx->streams[trackIndex];
    if (!st) return;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;

    if (av_new_packet(pkt, jsonData.size()) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, jsonData.constData(), jsonData.size());

    pkt->stream_index = trackIndex;
    pkt->pts      = av_rescale_q(ptsMs, {1, 1000}, st->time_base);
    pkt->dts      = pkt->pts;
    pkt->duration = av_rescale_q(1, {1, 1000}, st->time_base);

    writePacket(pkt);
    av_packet_free(&pkt);
}

AVStream* Muxer::getStream(int index) {
    // REMOVED LOCKER HERE: Reading nb_streams and streams is safe
    // after init() is finished and before close() starts.
    if (!m_outCtx || index < 0 || index >= (int)m_outCtx->nb_streams) {
        return nullptr;
    }
    return m_outCtx->streams[index];
}

void Muxer::close() {
    QMutexLocker locker(&m_mutex);

    // Stop accepting new packets, then DRAIN: signal the writer to finish and
    // join it. writerLoop keeps draining while the queue is non-empty even
    // after running==false, so every queued packet is written to m_outCtx
    // BEFORE we proceed to the trailer. Caller contract (ReplayManager):
    // workers are stopped+joined and the heartbeat is stopped before close(),
    // so no thread enqueues concurrently here.
    if (m_writerRunning.exchange(false)) {
        m_qCv.notify_all();
        if (m_writerThread.joinable()) m_writerThread.join();
    } else if (m_writerThread.joinable()) {
        // init() started a thread but writerRunning was already cleared
        // (e.g. a second close()): still join to avoid a dangling thread.
        m_writerThread.join();
    }

    // Defensive: on a clean close the writer drains fully, so the queue is
    // empty here. Free anything left only to guarantee no leak on an abnormal
    // path (would have been written before the trailer otherwise).
    {
        std::lock_guard<std::mutex> lk(m_qMutex);
        while (!m_pktQueue.empty()) {
            AVPacket* p = m_pktQueue.front();
            m_pktQueue.pop();
            av_packet_free(&p);
        }
    }

    if (m_initialized && m_outCtx) {
        // Writer thread has joined: this thread now solely owns m_outCtx.
        av_write_trailer(m_outCtx);
        if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_outCtx->pb);
        }
        avformat_free_context(m_outCtx);
        m_initialized = false;
        m_outCtx = nullptr;
        m_lastDts.clear();
    }
    m_telemetryTrackOffset = 0;
    m_telemetryTrackCount = 0;
}

QString Muxer::getVideoPath(QString fileName) {
    // During a session, return the path resolved at init(): re-running
    // the fallback logic could silently diverge from the open file.
    if (m_initialized && !m_activePath.isEmpty()) {
        return m_activePath;
    }

    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/videos";

    QString base = m_outputDir.trimmed();
    if (base.isEmpty()) base = fallback;

    if (QDir::isRelativePath(base)) {
        // Legacy/unnormalized configs can carry relative paths (incl. a
        // literal "~/..."); resolving them against the cwd would write
        // somewhere the user never sees.
        qWarning() << "Muxer: relative save location refused, using" << fallback
                   << "(configured:" << base << ")";
        base = fallback;
    }

    QDir dir(base);
    if (!dir.exists()) dir.mkpath(".");
    const QFileInfo baseInfo(base);
    if (!baseInfo.isDir() || !baseInfo.isWritable()) {
        qWarning() << "Muxer: save location unusable, falling back to"
                   << fallback << "(configured:" << base << ")";
        // Configured location unusable: fall back to the default
        base = fallback;
        QDir fb(base);
        if (!fb.exists()) fb.mkpath(".");
    }

    return base + "/" + fileName + ".mkv";
}
