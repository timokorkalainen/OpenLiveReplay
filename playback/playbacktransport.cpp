#include "playbacktransport.h"

PlaybackTransport::PlaybackTransport(QObject *parent)
    : QObject(parent), m_tickTimer(new QTimer(this))
{
    connect(m_tickTimer, &QTimer::timeout, this, &PlaybackTransport::onTick);
    m_tickTimer->setInterval(m_timerIntervalMs);
    m_tickTimer->setTimerType(Qt::PreciseTimer);
    m_currentPos = 0;
}

int64_t PlaybackTransport::currentPos() const {
    QMutexLocker locker(&m_mutex);
    return m_currentPos;
}

double PlaybackTransport::speed() const {
    return m_speed;
}

bool PlaybackTransport::isPlaying() const {
    return m_isPlaying;
}

int PlaybackTransport::fps() const {
    return m_fps;
}

void PlaybackTransport::setPlaying(bool playing) {
    if (m_isPlaying == playing) return;

    m_isPlaying = playing;
    if (m_isPlaying) {
        m_playStartPos = m_currentPos;
        m_playStartTime.start();
        m_tickTimer->start();
    } else {
        m_tickTimer->stop();
    }
    emit playingChanged(m_isPlaying);
}

void PlaybackTransport::setSpeed(double speed) {
    if (qFuzzyCompare(m_speed, speed)) return;
    speed = qRound(speed * 100.0) / 100.0;
    m_speed = speed;
    if (m_isPlaying) {
        m_playStartPos = currentPos();
        m_playStartTime.restart();
    }
    emit speedChanged(m_speed);
}

void PlaybackTransport::setFps(int fps) {
    if (fps <= 0) return;
    if (m_fps == fps) return;
    m_fps = fps;
    emit fpsChanged(m_fps);
}

void PlaybackTransport::seek(int64_t posMs) {
    int64_t newPos = 0;
    {
        QMutexLocker locker(&m_mutex);
        m_currentPos = qMax(int64_t(0), posMs);
        if (m_isPlaying) {
            m_playStartPos = m_currentPos;
            m_playStartTime.restart();
        }
        newPos = m_currentPos;
    }
    emit posChanged(newPos);
}

void PlaybackTransport::step(int frames) {
    // Step by exact frame duration based on configured FPS
    int fps = qMax(1, m_fps);
    int64_t stepSize = static_cast<int64_t>(frames * (1000.0 / fps));
    seek(currentPos() + stepSize);
}

void PlaybackTransport::onTick() {
    if (!m_isPlaying) return;

    int64_t newPos = 0;
    bool shouldStop = false;
    {
        QMutexLocker locker(&m_mutex);
        // Calculate position from play start to avoid cumulative drift
        int64_t elapsed = m_playStartTime.elapsed();
        m_currentPos = m_playStartPos + static_cast<int64_t>(elapsed * m_speed);
        // Bounds checking (prevent negative time)
        if (m_currentPos < 0) {
            m_currentPos = 0;
            shouldStop = true;
        }
        newPos = m_currentPos;
    }

    if (shouldStop) {
        setPlaying(false);
    }

    emit posChanged(newPos);
}
