#include "replaymanager.h"
#include <QDebug>
#include <QDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QtGlobal>

ReplayManager::ReplayManager(QObject *parent) : QObject(parent) {
    m_muxer = new Muxer();
    m_clock = new RecordingClock();
    m_heartbeat = new QTimer(this);
    connect(m_heartbeat, &QTimer::timeout, this, &ReplayManager::onTimerTick);
}

ReplayManager::~ReplayManager() {
    stopRecording();
    cleanupBlueEncoder();
    delete m_muxer;
}

void ReplayManager::setTelemetryFeeds(const QStringList &feedIds,
                                      const QStringList &feedNames,
                                      const QList<int> &telemetryDelaysMs) {
    QMutexLocker locker(&m_stateMutex);
    if (m_isRecording) {
        return;
    }

    m_telemetryFeedIds.clear();
    m_telemetryFeedNames.clear();
    m_telemetryDelaysMs.clear();
    m_telemetryFeedIndexById.clear();

    for (int i = 0; i < feedIds.size(); ++i) {
        const QString feedId = feedIds.at(i);
        if (feedId.isEmpty() || m_telemetryFeedIndexById.contains(feedId)) {
            continue;
        }

        const int feedIndex = m_telemetryFeedIds.size();
        m_telemetryFeedIndexById.insert(feedId, feedIndex);
        m_telemetryFeedIds.append(feedId);
        m_telemetryFeedNames.append(i < feedNames.size() ? feedNames.at(i) : QString());
        m_telemetryDelaysMs.append(i < telemetryDelaysMs.size() ? telemetryDelaysMs.at(i) : 0);
    }
}

bool ReplayManager::recordTelemetryEvent(const QString &feedId, const QJsonObject &payload) {
    QJsonObject recorded;
    int64_t effectiveMs = 0;
    {
        QMutexLocker locker(&m_stateMutex);
        if (!m_isRecording || !m_muxer || !m_clock) {
            return false;
        }

        const auto feedIt = m_telemetryFeedIndexById.constFind(feedId);
        if (feedIt == m_telemetryFeedIndexById.constEnd()) {
            return false;
        }

        const int feedIndex = feedIt.value();
        const int configuredDelayMs = feedIndex < m_telemetryDelaysMs.size()
            ? m_telemetryDelaysMs.at(feedIndex)
            : 0;
        const int delayMs = qBound(0, configuredDelayMs, 10000);
        const int64_t receiveMs = qMax<int64_t>(0, m_clock->elapsedMs());
        effectiveMs = receiveMs + delayMs;

        recorded = payload;
        recorded.insert(QStringLiteral("feedId"), feedId);
        recorded.insert(QStringLiteral("olrReceiveMs"), receiveMs);
        recorded.insert(QStringLiteral("olrEffectiveMs"), effectiveMs);
        recorded.insert(QStringLiteral("olrTelemetryDelayMs"), delayMs);

        m_muxer->writeTelemetryPacket(
            feedIndex,
            effectiveMs,
            QJsonDocument(recorded).toJson(QJsonDocument::Compact));
    }

    emit telemetryRecorded(feedId, recorded, effectiveMs);
    return true;
}

// ─── Blue-frame encoder for unmapped view tracks ───────────────────────
bool ReplayManager::setupBlueEncoder() {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!encoder) return false;

    m_blueEncCtx = avcodec_alloc_context3(encoder);
    if (!m_blueEncCtx) return false;

    m_blueEncCtx->width     = m_videoWidth;
    m_blueEncCtx->height    = m_videoHeight;
    m_blueEncCtx->time_base = {1, m_fps};
    m_blueEncCtx->framerate = {m_fps, 1};
    m_blueEncCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
    m_blueEncCtx->gop_size  = 1;
    m_blueEncCtx->bit_rate  = 30000000;

    if (avcodec_open2(m_blueEncCtx, encoder, nullptr) < 0) {
        avcodec_free_context(&m_blueEncCtx);
        return false;
    }

    m_blueFrame = av_frame_alloc();
    m_blueFrame->format = AV_PIX_FMT_YUV420P;
    m_blueFrame->width  = m_videoWidth;
    m_blueFrame->height = m_videoHeight;
    if (av_frame_get_buffer(m_blueFrame, 0) < 0) {
        av_frame_free(&m_blueFrame);
        avcodec_free_context(&m_blueEncCtx);
        return false;
    }

    // Paint solid blue (YUV).  Cb is clamped to 240 (broadcast-legal chroma
    // max) instead of the illegal 255, matching the StreamWorker blue paints
    // so every blue placeholder is identical.  Must run BEFORE the
    // encode-once below.
    memset(m_blueFrame->data[0], 128, m_blueFrame->linesize[0] * m_blueFrame->height);
    memset(m_blueFrame->data[1], 240, m_blueFrame->linesize[1] * (m_blueFrame->height / 2));
    memset(m_blueFrame->data[2], 107, m_blueFrame->linesize[2] * (m_blueFrame->height / 2));

    // Encode the blue frame ONCE and cache the compressed packet.  gop_size=1
    // (intra) means a single send yields one self-contained keyframe packet,
    // perfect to reuse for every unmapped view on every pulse.  writeBlueFrames
    // clones this and re-stamps pts/dts per write, so NO encode happens on the
    // GUI thread in the heartbeat hot path.
    m_blueFrame->pts = 0;
    if (avcodec_send_frame(m_blueEncCtx, m_blueFrame) != 0) {
        av_frame_free(&m_blueFrame);
        avcodec_free_context(&m_blueEncCtx);
        return false;
    }
    m_cachedBluePkt = av_packet_alloc();
    if (!m_cachedBluePkt) {
        av_frame_free(&m_blueFrame);
        avcodec_free_context(&m_blueEncCtx);
        return false;
    }
    int recvRet = avcodec_receive_packet(m_blueEncCtx, m_cachedBluePkt);
    if (recvRet == AVERROR(EAGAIN)) {
        // Intra encoder buffered the frame: drain it out.
        avcodec_send_frame(m_blueEncCtx, nullptr);
        recvRet = avcodec_receive_packet(m_blueEncCtx, m_cachedBluePkt);
    }
    if (recvRet != 0) {
        av_packet_free(&m_cachedBluePkt);
        av_frame_free(&m_blueFrame);
        avcodec_free_context(&m_blueEncCtx);
        return false;
    }

    return true;
}

void ReplayManager::cleanupBlueEncoder() {
    if (m_cachedBluePkt) {
        av_packet_free(&m_cachedBluePkt);
        m_cachedBluePkt = nullptr;
    }
    if (m_blueFrame) { av_frame_free(&m_blueFrame); m_blueFrame = nullptr; }
    if (m_blueEncCtx) { avcodec_free_context(&m_blueEncCtx); m_blueEncCtx = nullptr; }
}

// ─── Recording lifecycle ───────────────────────────────────────────────
void ReplayManager::startRecording() {
    QMutexLocker locker(&m_stateMutex);
    if (m_isRecording || m_sourceUrls.isEmpty()) return;

    // 1. Initialize Muxer with M view-tracks (not N source-tracks).
    //    Do this BEFORE starting the clock / stamping the epoch: a failure
    //    here must not leave a phantom advancing clock behind for the idle UI.
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_sessionFileName = m_baseFileName + "_" + timestamp;
    // Recordings go to the user-configured location (empty = default)
    m_muxer->setOutputDirectory(m_outputDir);
    const bool muxerReady = m_telemetryFeedIds.isEmpty()
        ? m_muxer->init(m_sessionFileName, m_viewCount, m_videoWidth, m_videoHeight, m_fps, m_viewNames)
        : m_muxer->init(m_sessionFileName, m_viewCount, m_videoWidth, m_videoHeight, m_fps, m_viewNames,
                        m_telemetryFeedIds, m_telemetryFeedNames);
    if (!muxerReady) {
        qDebug() << "ReplayManager: Failed to init Muxer with base name" << m_sessionFileName;
        return;
    }

    // 2. Setup the blue-frame encoder for unmapped views
    cleanupBlueEncoder();
    if (!setupBlueEncoder()) {
        qDebug() << "ReplayManager: Failed to init blue frame encoder.";
        m_muxer->close();
        return;
    }

    // 3. Setup the session clock — only now that init + encoder have succeeded,
    //    so a failure above never leaves a running clock / stamped epoch.
    if (m_clock) delete m_clock;
    m_clock = new RecordingClock();
    m_clock->start();
    m_recordingStartEpochMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    // 4. Launch one StreamWorker PER SOURCE (not per view).
    //    Workers capture from their URL and encode into whichever view-track
    //    they are currently mapped to (or skip encoding when m_viewTrack == -1).
    m_globalFrameCount = 0;
    m_blueAudioCursor = QVector<int64_t>(m_viewCount, -1);
    const int sourceCount = m_sourceUrls.size();

    for (int s = 0; s < sourceCount; ++s) {
        StreamWorker* worker = new StreamWorker(
            m_sourceUrls[s], s, m_muxer, m_clock,
            m_videoWidth, m_videoHeight, m_fps);

        // Set per-source metadata JSON for the subtitle track
        if (s < m_sourceMetadata.size()) {
            worker->setSourceMetadata(m_sourceMetadata[s]);
        }
        if (s < m_sourceTrims.size()) {
            worker->setTrimOffsetMs(m_sourceTrims[s]);
        }

        // The worker QObject must live on its own thread, otherwise the
        // queued masterPulse slot (jitter pull + encode + mux write) runs
        // on the MAIN thread: QThread object affinity is the creating
        // thread, and run()/exec() alone does not change it.
        worker->moveToThread(worker);

        connect(this, &ReplayManager::masterPulse,
                worker, &StreamWorker::onMasterPulse,
                Qt::QueuedConnection);

        // Relay the worker's connection-state transitions to the UI. The
        // worker emits from its capture thread, so deliver queued onto the
        // thread ReplayManager lives on (main); UIManager then receives it
        // there and updates its per-source connected state.
        connect(worker, &StreamWorker::connectionChanged, this,
                &ReplayManager::sourceConnectionChanged, Qt::QueuedConnection);

        m_workers.append(worker);
        worker->start(QThread::HighPriority);
    }

    // 5. Apply the initial view→source mapping
    updateViewMapping(m_viewSlotMap);

    // 6. Start the master heartbeat
    const int intervalMs = qMax(1, static_cast<int>(1000.0 / m_fps));
    m_heartbeat->start(intervalMs);
    m_isRecording = true;
    qDebug() << "ReplayManager: Recording started."
             << sourceCount << "sources," << m_viewCount << "views.";
}

void ReplayManager::stopRecording() {
    QMutexLocker locker(&m_stateMutex);
    if (!m_isRecording)
        return;

    m_heartbeat->stop();
    m_isRecording = false;

    for (int i = 0; i < m_workers.length(); i++) {
        m_workers[i]->stop();
    }

    for (int i = 0; i < m_workers.length(); i++) {
        m_workers[i]->wait();
        delete m_workers[i];
    }
    m_workers.clear();

    m_muxer->close();
    cleanupBlueEncoder();
    m_recordingStartEpochMs = 0;

    if (m_clock) {
        // Capture the final duration before deleting the clock so callers
        // (recordedDurationMs / scrubPosition) keep getting a valid value.
        m_lastKnownDurationMs = qMax<int64_t>(0, m_clock->elapsedMs());
        delete m_clock;
        m_clock = nullptr;
    }

    qDebug() << "ReplayManager: Recording stopped.";
}

// ─── View mapping: purely virtual, ZERO FFmpeg impact ──────────────────
//
// Mapping-transition behavior (a known minor, with a safety net):
//   This runs on the main thread and flips both m_viewSlotMap (read
//   synchronously by writeBlueFrames) and each worker's atomic view-track
//   (read by the worker when it PROCESSES a queued masterPulse — async).
//   Because writeBlueFrames decides at emit time while the worker decides
//   at process time, there is a one-tick skew at a transition:
//     • map-in:  writeBlueFrames stops writing blue for the view the instant
//                the mapping flips, but any masterPulses already QUEUED to
//                the worker before the flip are processed with the NEW
//                view-track, so the worker re-encodes a real frame for a
//                frame index whose blue placeholder was already written.
//                Result: a brief video duplicate (+ ~33 ms overlapping audio)
//                on that view track.
//     • unmap:   symmetric — a brief one-frame hole is possible.
//   The muxer's per-stream monotonic-DTS bump (Muxer::writePacket) absorbs
//   the duplicate without corrupting the file (the later packet is nudged to
//   lastDts+1 rather than dropped), and the audio cursors on both sides
//   (m_blueAudioCursor here, m_audioWriteCursor in StreamWorker) are anchored
//   to the SAME recording-time→48 kHz sample mapping, so silence tiles
//   seamlessly onto source audio across an unmap with no gap.
//   A "skip blue for one tick after map-in" mitigation was considered and
//   rejected: writeBlueFrames already stops the instant the mapping flips, so
//   the residual duplicate comes purely from worker BACKLOG (old frame
//   indices), which such a skip cannot address and would instead turn into a
//   hole.  Eliminating the 1-frame skew cleanly would require per-pulse
//   view-track snapshots in the worker — out of scope here; left as-is behind
//   the DTS-bump safety net.
void ReplayManager::updateViewMapping(const QList<int>& viewSlotMap) {
    m_viewSlotMap = viewSlotMap;

    // Build a reverse map: for each source, which view-track is it in?
    // Default is -1 (not assigned to any view).
    const int sourceCount = m_workers.size();
    QVector<int> sourceToTrack(sourceCount, -1);

    for (int v = 0; v < viewSlotMap.size(); ++v) {
        int src = viewSlotMap[v];
        if (src >= 0 && src < sourceCount) {
            sourceToTrack[src] = v;
        }
    }

    // Atomically update each worker's view-track assignment.
    // This is the ONLY thing that changes — no URL changes, no FFmpeg ops.
    for (int s = 0; s < sourceCount; ++s) {
        m_workers[s]->setViewTrack(sourceToTrack[s]);
    }

    qDebug() << "ReplayManager: View mapping updated:" << viewSlotMap;
}

// ─── Source URL change (real FFmpeg reconnect — for user editing a URL) ─
void ReplayManager::updateSourceUrl(int sourceIndex, const QString &url) {
    if (sourceIndex >= 0 && sourceIndex < m_sourceUrls.size()) {
        m_sourceUrls[sourceIndex] = url;
        if (m_isRecording && sourceIndex < m_workers.size()) {
            m_workers[sourceIndex]->changeSource(url);
        }
    }
}

void ReplayManager::updateSourceTrim(int sourceIndex, int ms) {
    if (sourceIndex < 0) return;
    if (sourceIndex < m_sourceTrims.size()) m_sourceTrims[sourceIndex] = ms;
    if (m_isRecording && sourceIndex < m_workers.size()) {
        m_workers[sourceIndex]->setTrimOffsetMs(ms);
    }
}

// ─── Master heartbeat ──────────────────────────────────────────────────
void ReplayManager::onTimerTick() {
    if (!m_clock) return;

    const int64_t elapsedMs = m_clock->elapsedMs();
    const int64_t derivedFrame = (elapsedMs * m_fps) / 1000;

    // Only emit when the frame count actually advances.
    if (derivedFrame <= m_globalFrameCount) return;

    // Emit one pulse PER FRAME so late timer ticks don't leave
    // frame-index holes in the video tracks.  Catch-up is capped at one
    // second of backlog (longer stalls resume from the current frame),
    // and drained a few frames per tick so the catch-up itself never
    // freezes the main thread — the remainder follows on later ticks.
    int64_t from = m_globalFrameCount + 1;
    if (derivedFrame - from >= m_fps) {
        from = derivedFrame - m_fps + 1;
    }
    const int64_t maxPerTick = qMax(1, m_fps / 4);
    const int64_t to = qMin(derivedFrame, from + maxPerTick - 1);
    for (int64_t f = from; f <= to; ++f) {
        m_globalFrameCount = f;
        const int64_t frameMs = (f * 1000) / m_fps;

        // 1. Emit masterPulse — source workers do jitter pull + encode
        emit masterPulse(f, frameMs);

        // 2. Write blue frames for any unmapped view-tracks
        writeBlueFrames(frameMs);
    }
}

void ReplayManager::writeBlueFrames(int64_t elapsedMs) {
    if (!m_blueEncCtx || !m_cachedBluePkt || !m_muxer) return;

    // Skip the encode entirely when every view has a source mapped —
    // but still reset the per-view silence cursors.
    bool anyUnmapped = false;
    for (int v = 0; v < m_viewCount; ++v) {
        if (v < m_viewSlotMap.size() && m_viewSlotMap[v] >= 0) {
            if (v < m_blueAudioCursor.size()) m_blueAudioCursor[v] = -1;
        } else {
            anyUnmapped = true;
        }
    }
    if (!anyUnmapped) return;

    // The blue video packet is static and was encoded ONCE in
    // setupBlueEncoder.  We reuse m_cachedBluePkt below — clone + re-stamp
    // pts/dts per view per pulse.  NO avcodec_send_frame/receive_packet here:
    // that's the whole point of the cache (the heartbeat can fire up to
    // fps/4 pulses per tick on the GUI thread).

    // Frame-count-derived recording time on the FILE timeline: same as
    // the source workers' audio cursor, so silence and source audio tile
    // seamlessly when a view's mapping changes.
    const int64_t recMs = (m_globalFrameCount * 1000) / m_fps;
    const int64_t silenceTargetEnd = recMs * StreamWorker::kAudioSampleRate / 1000;

    for (int v = 0; v < m_viewCount; ++v) {
        // Skip views that have a source assigned (those get real frames
        // from the source worker's processEncoderTick).  Reset the
        // silence cursor so an unmap later resumes at current time.
        if (v < m_viewSlotMap.size() && m_viewSlotMap[v] >= 0) {
            if (v < m_blueAudioCursor.size()) m_blueAudioCursor[v] = -1;
            continue;
        }

        AVPacket* pkt = av_packet_clone(m_cachedBluePkt);
        if (!pkt) continue;

        // RE-STAMP: the cached packet carries the fixed pts/dts from its
        // one-time encode.  Overwrite both with the CURRENT frame index on
        // the encoder timeline ({1, m_fps}) — exactly what the old per-pulse
        // path produced via m_blueFrame->pts = m_globalFrameCount — then
        // rescale to the stream timebase.  Without this the muxer's
        // monotonic-DTS bump would fire on every write.
        pkt->pts = m_globalFrameCount;
        pkt->dts = m_globalFrameCount;
        pkt->stream_index = v;
        AVStream* st = m_muxer->getStream(v);
        if (st) {
            av_packet_rescale_ts(pkt, m_blueEncCtx->time_base, st->time_base);
        }
        m_muxer->writePacket(pkt);
        av_packet_free(&pkt);

        // Write gap-free silence for unmapped views (PCM S16LE zero-fill).
        // Cursor-based: missed heartbeat ticks are filled on the next one
        // instead of leaving holes in the PCM track, and the same jitter
        // delay as source audio keeps mapping transitions seamless.
        if (silenceTargetEnd > 0 && v < m_blueAudioCursor.size()) {
            int64_t &cursor = m_blueAudioCursor[v];
            if (cursor < 0) {
                cursor = qMax<int64_t>(0, silenceTargetEnd
                                          - StreamWorker::kAudioSampleRate / m_fps);
            }
            if (silenceTargetEnd > cursor) {
                const int64_t n = qMin<int64_t>(silenceTargetEnd - cursor,
                                                StreamWorker::kAudioSampleRate);
                const int silenceBytes = int(n) * StreamWorker::kAudioBytesPerSample;
                int audioTrackIdx = m_muxer->audioTrackOffset() + v;
                AVPacket* aPkt = av_packet_alloc();
                if (aPkt && av_new_packet(aPkt, silenceBytes) == 0) {
                    memset(aPkt->data, 0, silenceBytes);
                    aPkt->stream_index = audioTrackIdx;
                    AVStream* aSt = m_muxer->getStream(audioTrackIdx);
                    if (aSt) {
                        aPkt->pts = av_rescale_q(cursor,
                            {1, StreamWorker::kAudioSampleRate}, aSt->time_base);
                        aPkt->dts = aPkt->pts;
                        aPkt->duration = av_rescale_q(n,
                            {1, StreamWorker::kAudioSampleRate}, aSt->time_base);
                    }
                    m_muxer->writePacket(aPkt);
                }
                if (aPkt) av_packet_free(&aPkt);
                cursor += n;
            }
        }

        // Write an empty metadata subtitle for unmapped views
        static const QByteArray emptyMeta("{}");
        m_muxer->writeMetadataPacket(v, elapsedMs, emptyMeta);
    }
    // NOTE: m_cachedBluePkt is NOT freed here — it is session-scoped and
    // reused every pulse; cleanupBlueEncoder() frees it at teardown.
}

// ─── Utility ───────────────────────────────────────────────────────────
int64_t ReplayManager::getElapsedMs() {
    // While recording the live clock is authoritative; after stop it is gone,
    // so fall back to the duration captured at stopRecording. Never return -1
    // (that produced garbage snapshot timecodes / QML binding values).
    QMutexLocker locker(&m_stateMutex);
    if (m_clock) return qMax<int64_t>(0, m_clock->elapsedMs());
    return m_lastKnownDurationMs;
}

QString ReplayManager::getVideoPath() {
    if (!m_sessionFileName.isEmpty()) {
        return m_muxer->getVideoPath(m_sessionFileName);
    }
    return m_muxer->getVideoPath(m_baseFileName);
}
