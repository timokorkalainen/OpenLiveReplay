#include "replaymanager.h"
#include <QDebug>
#include <QUrl>

ReplayManager::ReplayManager(QObject *parent) : QObject(parent) {
    m_muxer = new Muxer();
    // Default to a 4-track setup
    setTrackCount(4);
}

ReplayManager::~ReplayManager() {
    stopRecording();
    delete m_muxer;
}
int ReplayManager::trackCount() const {
    return m_trackUrls.size();
}

QStringList ReplayManager::trackUrls() const {
    return m_trackUrls;
}

bool ReplayManager::isRecording() const {
    return m_isRecording;
}

void ReplayManager::setTrackCount(int count) {
    if (m_isRecording || count < 1 || count > 16) return;

    m_trackUrls.clear();
    for (int i = 0; i < count; ++i) {
        m_trackUrls << "";
    }
    emit trackCountChanged();
    emit trackUrlsChanged();
}

void ReplayManager::setTrackUrl(int index, const QString &url) {
    if (m_isRecording || index < 0 || index >= m_trackUrls.size()) return;

    m_trackUrls[index] = url;
    emit trackUrlsChanged();
}

void ReplayManager::applyTrackSource(int index, const QString &url) {
    if (index < 0 || index >= m_trackUrls.size()) return;

    m_trackUrls[index] = url;
    emit trackUrlsChanged();

    if (m_isRecording) {

        if (m_workers.contains(index)) {
            // 1. Capture the exact progress of the old worker before killing it
            m_workers[index]->stop();
            m_workers[index]->wait();
            delete m_workers[index];
        }

        // 2. Start new worker with the offset
        StreamWorker *worker = new StreamWorker(url, index, m_muxer, m_clock);
        m_workers[index] = worker;
        worker->start(QThread::HighPriority);
    }
}

// Helper to centralize worker creation
void ReplayManager::startWorker(int index, const QString &url) {
    StreamWorker *worker = new StreamWorker(url, index, m_muxer, m_clock);

    connect(this, &ReplayManager::masterPulse,
            worker, &StreamWorker::onMasterPulse,
            Qt::QueuedConnection);

    m_workers[index] = worker;

    // Set priority high to ensure the 30fps pulse is rock-solid on macOS
    worker->start(QThread::HighPriority);
}

void ReplayManager::startRecording(const QString &path) {
    if (m_isRecording) return;

    if (!m_muxer->init(path, m_trackUrls.size())) return;

    m_globalFrameCount = 0;
    m_clock = new RecordingClock(this);
    m_clock->start();

    for (int i = 0; i < m_trackUrls.size(); ++i) {
        if (!m_trackUrls[i].isEmpty()) {
            startWorker(i, m_trackUrls[i]);
        }
    }

    m_heartbeat = new QTimer(this);
    m_heartbeat->setTimerType(Qt::PreciseTimer);
    connect(m_heartbeat, &QTimer::timeout, this, &ReplayManager::onTimerTick);

    // Start the 30fps pulse (33.33ms)
    m_heartbeat->start(33);

    m_isRecording = true;
    emit isRecordingChanged();
}

void ReplayManager::onTimerTick() {
    // Single point of truth: Every worker encodes exactly this frame index
    int64_t streamTimeMs = m_clock->elapsedMs();
    emit masterPulse(m_globalFrameCount++, streamTimeMs);
}

int64_t ReplayManager::currentStreamTimeMs() {
    // Returns 0 at the start, 1000 after 1 second, etc.
    return m_clock->elapsedMs();
}

void ReplayManager::stopRecording() {
    if (!m_isRecording) return;


    // Stop the heartbeats
    m_heartbeat->stop();

    // Stop all workers
    for (StreamWorker* worker : m_workers.values()) {
        worker->stop();
        worker->wait();
        delete worker;
    }
    m_workers.clear();

    // Finalize file (writes duration/cues)
    m_muxer->close();

    m_isRecording = false;

    // delete the clock
    delete m_clock;
    m_clock = nullptr;

    emit isRecordingChanged();
}
