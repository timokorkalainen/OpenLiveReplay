#include "replaymanager.h"
#include <QDebug>
#include <QUrl>
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
    delete m_muxer;
}

void ReplayManager::startRecording() {
    if (m_isRecording || m_trackUrls.isEmpty()) return;

    // 1. Setup the session clock
    if (m_clock) delete m_clock;

    m_clock = new RecordingClock();
    m_clock->start();

    // 2. Initialize Muxer with timestamped filename to avoid overwrites
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_sessionFileName = m_baseFileName + "_" + timestamp;
    if (!m_muxer->init(m_sessionFileName, m_trackUrls.size(), m_videoWidth, m_videoHeight, m_fps, m_streamNames)) {
        qDebug() << "ReplayManager: Failed to init Muxer with base name" << m_sessionFileName;
        return;
    }

    // 3. Launch Workers
    m_globalFrameCount = 0;
    for (int i = 0; i < m_trackUrls.size(); ++i) {
        StreamWorker* worker = new StreamWorker(m_trackUrls[i], i, m_muxer, m_clock,
                            m_videoWidth, m_videoHeight, m_fps);

        // Connect the pulse signal using QueuedConnection for thread safety
        connect(this, &ReplayManager::masterPulse,
                worker, &StreamWorker::onMasterPulse,
                Qt::QueuedConnection);

        m_workers.append(worker);
        worker->start(QThread::HighPriority);
    }

    const int intervalMs = qMax(1, static_cast<int>(1000.0 / m_fps));
    m_heartbeat->start(intervalMs);
    m_isRecording = true;
    qDebug() << "ReplayManager: Recording started.";
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

    // Safe to delete clock after all workers have stopped accessing it
    if (m_clock) {
        delete m_clock;
        m_clock = nullptr;
    }

    qDebug() << "ReplayManager: Recording stopped.";
}

void ReplayManager::updateTrackUrl(int index, const QString &url) {
    if (index >= 0 && index < m_trackUrls.size()) {
        m_trackUrls[index] = url;
        // If recording, signal the specific worker to hot-swap
        if (m_isRecording && index < m_workers.size()) {
            m_workers[index]->changeSource(url);
        }
    }
}

void ReplayManager::onTimerTick() {
    if (!m_clock) return;

    // Derive frame index from elapsed time to avoid drift
    const int64_t elapsedMs = m_clock->elapsedMs();
    const int64_t derivedFrame = (elapsedMs * m_fps) / 1000;
    if (derivedFrame > m_globalFrameCount) {
        m_globalFrameCount = derivedFrame;
    }
    emit masterPulse(m_globalFrameCount, elapsedMs);
}

QString ReplayManager::getFullOutputPath() {
    QDir dir(m_outputDir);
    // Combines directory and filename safely for the current OS
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
