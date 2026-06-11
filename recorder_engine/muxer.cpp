#include "muxer.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

Muxer::Muxer() {}

Muxer::~Muxer() { close(); }

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
                 int audioSampleRate, int audioChannels) {
    QMutexLocker locker(&m_mutex);

    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;
    if (fps <= 0) fps = 30;

    // 1. Create Format Context for Matroska
    m_activePath = getVideoPath(filename);
    avformat_alloc_output_context2(&m_outCtx, nullptr, "matroska", m_activePath.toUtf8().constData());
    if (!m_outCtx) return false;

    // 2. Pre-allocate Video Tracks
    for (int i = 0; i < videoTrackCount; i++) {
        AVStream* st = avformat_new_stream(m_outCtx, nullptr);
        st->id = i;

        // 1. Set parameters
        st->codecpar->codec_id = AV_CODEC_ID_MPEG2VIDEO;
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
        return false;
    }
    avio_flush(m_outCtx->pb); // Forces the EBML header to be visible to the reader

    m_lastDts.clear();
    m_lastFlush.start();

    m_initialized = true;
    return true;
}

void Muxer::writePacket(AVPacket* pkt) {
    QMutexLocker locker(&m_mutex);
    if (!m_initialized || !m_outCtx) return;

    // Ensure monotonic DTS per stream — bump forward if needed.
    // Dropping was too aggressive: when a source was re-mapped to a view
    // track that had blue-frame DTS ahead of the source encoder's counter,
    // every packet was silently lost.  Bumping preserves the data.
    int idx = pkt->stream_index;
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
    AVPacket* localPkt = av_packet_clone(pkt);
    if (!localPkt) return;

    int ret = av_write_frame(m_outCtx, localPkt);
    av_packet_free(&localPkt); // av_write_frame does NOT take ownership

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
    if (m_initialized && m_outCtx) {
        av_write_trailer(m_outCtx);
        if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_outCtx->pb);
        }
        avformat_free_context(m_outCtx);
        m_initialized = false;
        m_outCtx = nullptr;
        m_lastDts.clear();
    }
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

