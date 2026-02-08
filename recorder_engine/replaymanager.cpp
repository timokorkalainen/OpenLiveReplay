#include "replaymanager.h"
#include <QDebug>
#include <QDir>
#include <QDateTime>

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

    // Paint solid blue (YUV 128 / 255 / 107)
    memset(m_blueFrame->data[0], 128, m_blueFrame->linesize[0] * m_blueFrame->height);
    memset(m_blueFrame->data[1], 255, m_blueFrame->linesize[1] * (m_blueFrame->height / 2));
    memset(m_blueFrame->data[2], 107, m_blueFrame->linesize[2] * (m_blueFrame->height / 2));

    return true;
}

void ReplayManager::cleanupBlueEncoder() {
    if (m_blueFrame) { av_frame_free(&m_blueFrame); m_blueFrame = nullptr; }
    if (m_blueEncCtx) { avcodec_free_context(&m_blueEncCtx); m_blueEncCtx = nullptr; }
}

// ─── Recording lifecycle ───────────────────────────────────────────────
void ReplayManager::startRecording() {
    if (m_isRecording || m_sourceUrls.isEmpty()) return;

    // 1. Setup the session clock
    if (m_clock) delete m_clock;
    m_clock = new RecordingClock();
    m_clock->start();
    m_recordingStartEpochMs = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();

    // 2. Initialize Muxer with M view-tracks (not N source-tracks)
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_sessionFileName = m_baseFileName + "_" + timestamp;
    if (!m_muxer->init(m_sessionFileName, m_viewCount, m_videoWidth, m_videoHeight, m_fps, m_viewNames)) {
        qDebug() << "ReplayManager: Failed to init Muxer with base name" << m_sessionFileName;
        return;
    }

    // 3. Setup the blue-frame encoder for unmapped views
    cleanupBlueEncoder();
    if (!setupBlueEncoder()) {
        qDebug() << "ReplayManager: Failed to init blue frame encoder.";
        m_muxer->close();
        return;
    }

    // 4. Launch one StreamWorker PER SOURCE (not per view).
    //    Workers capture from their URL and encode into whichever view-track
    //    they are currently mapped to (or skip encoding when m_viewTrack == -1).
    m_globalFrameCount = 0;
    const int sourceCount = m_sourceUrls.size();

    for (int s = 0; s < sourceCount; ++s) {
        StreamWorker* worker = new StreamWorker(
            m_sourceUrls[s], s, m_muxer, m_clock,
            m_videoWidth, m_videoHeight, m_fps);

        connect(this, &ReplayManager::masterPulse,
                worker, &StreamWorker::onMasterPulse,
                Qt::QueuedConnection);

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
        delete m_clock;
        m_clock = nullptr;
    }

    qDebug() << "ReplayManager: Recording stopped.";
}

// ─── View mapping: purely virtual, ZERO FFmpeg impact ──────────────────
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

// ─── Master heartbeat ──────────────────────────────────────────────────
void ReplayManager::onTimerTick() {
    if (!m_clock) return;

    const int64_t elapsedMs = m_clock->elapsedMs();
    const int64_t derivedFrame = (elapsedMs * m_fps) / 1000;

    // Only emit when the frame count actually advances.
    if (derivedFrame <= m_globalFrameCount) return;

    m_globalFrameCount = derivedFrame;

    // 1. Emit masterPulse — source workers do jitter pull + encode
    emit masterPulse(m_globalFrameCount, elapsedMs);

    // 2. Write blue frames for any unmapped view-tracks
    writeBlueFrames();
}

void ReplayManager::writeBlueFrames() {
    if (!m_blueEncCtx || !m_blueFrame || !m_muxer) return;

    // Encode one blue frame per tick (all unmapped views share the same packet data)
    m_blueFrame->pts = m_globalFrameCount;
    if (avcodec_send_frame(m_blueEncCtx, m_blueFrame) != 0) return;

    AVPacket* basePkt = av_packet_alloc();
    if (avcodec_receive_packet(m_blueEncCtx, basePkt) != 0) {
        av_packet_free(&basePkt);
        return;
    }

    for (int v = 0; v < m_viewCount; ++v) {
        // Skip views that have a source assigned (those get real frames
        // from the source worker's processEncoderTick).
        if (v < m_viewSlotMap.size() && m_viewSlotMap[v] >= 0) continue;

        AVPacket* pkt = av_packet_clone(basePkt);
        if (!pkt) continue;

        pkt->stream_index = v;
        AVStream* st = m_muxer->getStream(v);
        if (st) {
            av_packet_rescale_ts(pkt, m_blueEncCtx->time_base, st->time_base);
        }
        m_muxer->writePacket(pkt);
        av_packet_free(&pkt);
    }

    av_packet_free(&basePkt);
}

// ─── Utility ───────────────────────────────────────────────────────────
QString ReplayManager::getFullOutputPath() {
    QDir dir(m_outputDir);
    return dir.absoluteFilePath(m_baseFileName + ".mkv");
}

int64_t ReplayManager::getElapsedMs() {
    if(!m_clock) return -1;
    return m_clock->elapsedMs();
}

QString ReplayManager::getVideoPath() {
    if (!m_sessionFileName.isEmpty()) {
        return m_muxer->getVideoPath(m_sessionFileName);
    }
    return m_muxer->getVideoPath(m_baseFileName);
}
