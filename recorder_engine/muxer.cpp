#include "muxer.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QRegularExpression>

Muxer::Muxer() {}

Muxer::~Muxer() { close(); }

namespace {
// True iff `tc` is a well-formed SMPTE timecode "HH:MM:SS:FF" (or ';' before the
// frames field for drop-frame). Empty/malformed -> false, so no tag is written
// and a no-TC recording stays byte-identical. Deliberately a shape check only;
// the caller has already chosen the value.
bool isWellFormedTimecode(const QString& tc) {
    static const QRegularExpression re(QStringLiteral("^\\d{2}:\\d{2}:\\d{2}[:;]\\d{2}$"));
    return re.match(tc).hasMatch();
}
} // namespace

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps,
                 const QStringList& streamNames, int audioSampleRate, int audioChannels,
                 VideoCodecChoice codec, const QByteArray& videoExtradata,
                 const QString& startTimecode, int fpsNum, int fpsDen) {
    return init(filename, videoTrackCount, width, height, fps, streamNames, {}, {}, audioSampleRate,
                audioChannels, codec, videoExtradata, startTimecode, fpsNum, fpsDen);
}

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps,
                 const QStringList& streamNames, int audioSampleRate, int audioChannels,
                 const QString& startTimecode, int fpsNum, int fpsDen) {
    return init(filename, videoTrackCount, width, height, fps, streamNames, {}, {}, audioSampleRate,
                audioChannels, VideoCodecChoice::Mpeg2Software, {}, startTimecode, fpsNum, fpsDen);
}

bool Muxer::init(const QString& filename, int videoTrackCount, int width, int height, int fps,
                 const QStringList& streamNames, const QStringList& telemetryFeedIds,
                 const QStringList& telemetryFeedNames, int audioSampleRate, int audioChannels,
                 VideoCodecChoice codec, const QByteArray& videoExtradata,
                 const QString& startTimecode, int fpsNum, int fpsDen) {
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
    // Advertised rational frame rate: prefer the explicit fpsNum/fpsDen (e.g.
    // 30000/1001 for 29.97); fall back to the integer {fps, 1} when unset (0/0).
    const AVRational advertisedRate =
        (fpsNum > 0 && fpsDen > 0) ? AVRational{fpsNum, fpsDen} : AVRational{fps, 1};

    // Session start timecode candidate. The "timecode" tag is NOT written here:
    // the header write is deferred to the first muxed packet (ensureHeaderWritten),
    // where the winning candidate is materialised into the tag — this is what lets
    // a LIVE recording, which has observed no TC at start, still carry the first
    // muxed frame's TC. An up-front candidate (passed here, e.g. from a unit test)
    // is the initial candidate; workers may supply one later via
    // setStartTimecodeCandidate. Empty or malformed -> no tag (byte-identical).
    {
        QMutexLocker headerLock(&m_headerMutex);
        m_headerWritten = false;
        m_startTimecodeCandidate = isWellFormedTimecode(startTimecode) ? startTimecode : QString();
        // Open the bounded grace window for the first source TC (see muxer.h). An
        // up-front candidate (e.g. from a unit test) means TC is already known, so
        // there is nothing to wait for — leave the grace at 0 and commit on the
        // first packet exactly as before. Tunable via OLR_MUXER_TMCD_GRACE_MS.
        m_headerGraceMs = 0;
        if (m_startTimecodeCandidate.isEmpty()) {
            m_headerGraceMs = 750;
            bool graceOk = false;
            const QByteArray graceEnv = qgetenv("OLR_MUXER_TMCD_GRACE_MS");
            if (!graceEnv.isEmpty()) {
                const int parsed = graceEnv.toInt(&graceOk);
                if (graceOk && parsed >= 0) m_headerGraceMs = parsed;
            }
        }
        m_headerGraceTimer.restart();
    }

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
            st->codecpar->extradata_size = static_cast<int>(videoExtradata.size());
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->width = width;
        st->codecpar->height = height;
        st->codecpar->format = AV_PIX_FMT_YUV420P;
        st->codecpar->bit_rate = 30000000;

        // 2. Use millisecond timebase for Matroska
        // This keeps timeline duration consistent across players.
        st->time_base = {1, 1000};

        // 3. Set the metadata hints (advertised rate; ms time_base is unchanged)
        st->avg_frame_rate = advertisedRate;
        st->r_frame_rate = advertisedRate;

        // For MPEG-2, you can also set the 'closed gop' and 'fixed fps' flags in codecpar
        st->codecpar->video_delay = 0;

        // Always name video tracks generically: "Track 1", "Track 2", ...
        const QString trackTitle = QString("Track %1").arg(i + 1);
        av_dict_set(&st->metadata, "title", trackTitle.toUtf8().constData(), 0);

        // The per-track "timecode" tag is set later, in ensureHeaderWritten(),
        // from the winning start-timecode candidate (deferred header write).
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
    m_telemetryTrackCount = static_cast<int>(telemetryFeedIds.size());
    for (int i = 0; i < telemetryFeedIds.size(); ++i) {
        AVStream* st = avformat_new_stream(m_outCtx, nullptr);
        st->id = m_telemetryTrackOffset + i;
        st->codecpar->codec_id = AV_CODEC_ID_TEXT;
        st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
        st->time_base = {1, 1000};

        const QString& feedId = telemetryFeedIds.at(i);
        const QString feedName = i < telemetryFeedNames.size() ? telemetryFeedNames.at(i) : QString();
        const QString title = QString("Feed %1 Telemetry").arg(feedId);
        av_dict_set(&st->metadata, "title", title.toUtf8().constData(), 0);
        av_dict_set(&st->metadata, "olr_track_type", "feed_telemetry", 0);
        av_dict_set(&st->metadata, "olr_feed_id", feedId.toUtf8().constData(), 0);
        av_dict_set(&st->metadata, "olr_feed_name", feedName.toUtf8().constData(), 0);
    }

    // 3. Set Matroska specific options for Chase Play. STORED, not consumed:
    // the deferred avformat_write_header (ensureHeaderWritten) applies these on
    // the first packet. m_headerOpts is owned by the Muxer until then (freed in
    // ensureHeaderWritten or, on a never-written header, in close()).
    av_dict_free(&m_headerOpts); // defensive: clear any stale opts from a prior init
    m_headerOpts = nullptr;
    av_dict_set(&m_headerOpts, "reserve_index_space", "1024k",
                0); // Crucial for seeking while recording
    av_dict_set(&m_headerOpts, "cluster_size_limit", "1M", 0);  // Flush data often
    av_dict_set(&m_headerOpts, "cluster_time_limit", "100", 0); // Flush data to disk every 100ms
    av_dict_set(&m_headerOpts, "live", "1", 0); // Signal this is a live-streamed file

    // 3b. Store recording start time metadata (UTC ISO-8601)
    const QString startIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    av_dict_set(&m_outCtx->metadata, "recording_start_time", startIso.toUtf8().constData(), 0);

    // 3c. The format-level "timecode" tag is NOT set here. It is materialised by
    // ensureHeaderWritten() on the first packet from the winning start-timecode
    // candidate (deferred header write), so live recordings carry a real TC.

    // 4. Open the output file NOW (so a bad path still fails init() exactly as
    //    before), but DEFER avformat_write_header to the first packet.
    if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_outCtx->pb, m_outCtx->url, AVIO_FLAG_WRITE) < 0) {
            av_dict_free(&m_headerOpts);
            m_headerOpts = nullptr;
            avformat_free_context(m_outCtx);
            m_outCtx = nullptr;
            resetTelemetryTracks();
            return false;
        }
    }

    m_lastDts.clear();
    m_lastFlush.start();
    {
        std::lock_guard<std::mutex> lk(m_fatalMsgMutex);
        m_fatalWriteMsg.clear();
    }
    m_fatalWriteError.store(false, std::memory_order_relaxed);
    m_consecutiveWriteErrors = 0;

    m_initialized = true;

    // Start the dedicated writer thread. The header has NOT been written yet, but
    // every write path calls ensureHeaderWritten() before enqueuing a packet, so
    // by the time the writer thread pops anything the header is in place. From here
    // until close() joins it, the writer thread is the ONLY thread that calls
    // av_write_frame/avio_flush on m_outCtx; the header write itself happens on the
    // ENQUEUEING (caller) thread, before the packet is handed off, so it never
    // races the writer thread.
    m_writerRunning = true;
    m_writerThread = std::thread(&Muxer::writerLoop, this);

    return true;
}

bool Muxer::headerWriteDeferred() {
    QMutexLocker headerLock(&m_headerMutex);
    // Defer only while: header still unwritten, no candidate has won yet, and the
    // grace window is still open. A registered candidate or an expired grace commits.
    if (m_headerWritten) return false;
    if (m_headerGraceMs <= 0) return false;
    if (!m_startTimecodeCandidate.isEmpty()) return false;
    return m_headerGraceTimer.isValid() && m_headerGraceTimer.elapsed() < m_headerGraceMs;
}

bool Muxer::ensureHeaderWritten() {
    QMutexLocker headerLock(&m_headerMutex);
    if (m_headerWritten) return true;
    if (!m_outCtx) return false;

    // Materialise the winning start-timecode candidate into the "timecode" tag,
    // format-level + each video track, IMMEDIATELY before the header is written
    // (the matroska muxer serialises metadata at header time). Empty/malformed
    // candidate -> no tag (a no-TC recording stays byte-identical to before).
    if (isWellFormedTimecode(m_startTimecodeCandidate)) {
        const QByteArray timecodeTag = m_startTimecodeCandidate.toUtf8();
        av_dict_set(&m_outCtx->metadata, "timecode", timecodeTag.constData(), 0);
        for (unsigned int i = 0; i < m_outCtx->nb_streams; ++i) {
            AVStream* st = m_outCtx->streams[i];
            if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                av_dict_set(&st->metadata, "timecode", timecodeTag.constData(), 0);
            }
        }
    }

    const int headerRet = avformat_write_header(m_outCtx, &m_headerOpts);
    av_dict_free(&m_headerOpts);
    m_headerOpts = nullptr;
    if (headerRet < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(headerRet, errbuf, sizeof(errbuf));
        qDebug() << "Muxer: avformat_write_header failed:" << errbuf;
        return false;
    }
    avio_flush(m_outCtx->pb); // Forces the EBML header to be visible to the reader
    m_headerWritten = true;
    return true;
}

void Muxer::setStartTimecodeCandidate(const QString& tc) {
    QMutexLocker headerLock(&m_headerMutex);
    // First valid candidate before the header is written wins (so the SAME thread
    // that writes the first packet supplies the start TC — no cross-thread race).
    if (!m_headerWritten && m_startTimecodeCandidate.isEmpty() && isWellFormedTimecode(tc)) {
        m_startTimecodeCandidate = tc;
    }
}

void Muxer::writePacket(AVPacket* pkt) {
    // ENQUEUE-ONLY. Clone the caller's packet (the caller still owns theirs,
    // exactly as before) and hand the clone to the writer thread, then return
    // immediately. The DTS-bump, av_write_frame and avio_flush all happen on
    // the writer thread — so a stalled disk no longer blocks the caller.
    if (!m_writerRunning.load(std::memory_order_acquire)) return;

    // Deferred header: NOT committed here anymore. The writer thread commits the
    // header (ensureHeaderWritten) just before it writes the first packet, after
    // honouring the bounded grace window (headerWriteDeferred) so the first source
    // TC can win the tmcd tag. Enqueue-only here keeps the producer non-blocking
    // and lets the writer hold early no-TC packets without dropping or reordering.

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

void Muxer::recordWriteOutcome(bool failed, const char* errLabel) {
    if (failed) {
        ++m_consecutiveWriteErrors;
        if (m_consecutiveWriteErrors >= kFatalWriteThreshold &&
            !m_fatalWriteError.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lk(m_fatalMsgMutex);
                m_fatalWriteMsg = errLabel ? errLabel : "write error";
            }
            m_fatalWriteError.store(true, std::memory_order_release);
        }
    } else {
        m_consecutiveWriteErrors = 0;
    }
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
            // Hold the first packet(s) while the header write is deferred for the
            // first source TC (bounded grace). Leave them queued — no pop, no
            // reorder, no drop — and re-loop after a short sleep until the grace
            // resolves (a candidate arrives or the window expires). Only honour the
            // deferral while still running; on shutdown drain immediately so close()
            // never wedges. Released the q lock first so setStartTimecodeCandidate /
            // ensureHeaderWritten (both take m_headerMutex) never block the producer.
            if (m_writerRunning.load(std::memory_order_acquire) && headerWriteDeferred()) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            pkt = m_pktQueue.front();
            m_pktQueue.pop();
        }
        // Notify a possibly back-pressured producer that there is now room.
        m_qCv.notify_one();

        // Commit the deferred header (once) just before the first real write, now
        // that the grace has resolved and any first-frame TC candidate has been
        // registered. On a fatal header failure record the outcome and drop the
        // packet (file is unusable if the header never landed).
        if (!ensureHeaderWritten()) {
            av_packet_free(&pkt);
            recordWriteOutcome(true, "avformat_write_header failed");
            continue;
        }

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
            recordWriteOutcome(true, errbuf);
        } else {
            recordWriteOutcome(false, nullptr);
        }

        // Flush at most every ~100 ms: keeps the chase-play reader within a
        // cluster of the live edge without a disk flush per packet.
        if (!m_lastFlush.isValid() || m_lastFlush.elapsed() >= 100) {
            avio_flush(m_outCtx->pb);
            if (m_outCtx->pb && m_outCtx->pb->error != 0) {
                recordWriteOutcome(true, "avio flush error");
            }
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

    if (av_new_packet(pkt, static_cast<int>(jsonData.size())) < 0) {
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

    if (av_new_packet(pkt, static_cast<int>(jsonData.size())) < 0) {
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
        // Empty-recording edge: if NO packet was ever written, the deferred header
        // is still unwritten. Write it now (no TC candidate is fine — empty tag),
        // so the file is a valid (empty) MKV exactly as a no-packet recording was
        // before the header became deferred. av_write_trailer requires a written
        // header, so only emit the trailer once the header is confirmed present.
        const bool headerOk = ensureHeaderWritten();
        if (headerOk) {
            av_write_trailer(m_outCtx);
        }
        if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m_outCtx->pb);
        }
        avformat_free_context(m_outCtx);
        m_initialized = false;
        m_outCtx = nullptr;
        m_lastDts.clear();
    }
    // Any header opts not consumed by a header write (e.g. write_header never
    // succeeded) are freed here so they never leak across sessions.
    av_dict_free(&m_headerOpts);
    m_headerOpts = nullptr;
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
