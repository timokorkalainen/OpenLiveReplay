#include "muxer.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

Muxer::Muxer() {}

Muxer::~Muxer() { close(); }

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames) {
    QMutexLocker locker(&m_mutex);

    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;
    if (fps <= 0) fps = 30;

    // 1. Create Format Context for Matroska
    avformat_alloc_output_context2(&m_outCtx, nullptr, "matroska", getVideoPath(filename).toUtf8().constData());
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

        // 2. CRITICAL: Set the stream timebase to match the ENCODER first
        // This tells FFmpeg: "The data coming in is at 30fps"
        st->time_base = {1, fps};

        // 3. Set the metadata hints
        st->avg_frame_rate = {fps, 1};
        st->r_frame_rate = {fps, 1};

        // For MPEG-2, you can also set the 'closed gop' and 'fixed fps' flags in codecpar
        st->codecpar->video_delay = 0;

        if (i < streamNames.size()) {
            const QString name = streamNames[i].trimmed();
            if (!name.isEmpty()) {
                av_dict_set(&st->metadata, "title", name.toUtf8().constData(), 0);
            }
        }
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
            return false;
        }
    }

    if (avformat_write_header(m_outCtx, &opts) < 0) return false;
    avio_flush(m_outCtx->pb); // Forces the EBML header to be visible to the reader

    m_lastDts = new QMap<int, int64_t>();

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
    if (m_lastDts->contains(idx) && pkt->dts <= (*m_lastDts)[idx]) {
        pkt->dts = (*m_lastDts)[idx] + 1;
        if (pkt->pts < pkt->dts) pkt->pts = pkt->dts;
    }
    (*m_lastDts)[idx] = pkt->dts;

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

    avio_flush(m_outCtx->pb);
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
    }
    if (m_lastDts) {
        delete m_lastDts;
        m_lastDts = nullptr;
    }
}

QString Muxer::getVideoPath(QString fileName) {
    // 1. Get the Documents directory for your app
    QString docPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

    // 2. Create a subfolder if you want to be organized
    QDir dir(docPath);
    if (!dir.exists("videos")) {
        dir.mkdir("videos");
    }

    // 3. Construct the full filename
    return docPath + "/videos/" + fileName+".mkv";
}

