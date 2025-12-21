#include "playbacktransport.h"

PlaybackTransport::PlaybackTransport(QObject *parent)
    : QObject(parent), m_tickTimer(new QTimer(this))
{
    connect(m_tickTimer, &QTimer::timeout, this, &PlaybackTransport::onTick);
    m_tickTimer->setInterval(m_timerIntervalMs);
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

void PlaybackTransport::setPlaying(bool playing) {
    if (m_isPlaying == playing) return;

    m_isPlaying = playing;
    if (m_isPlaying) {
        m_frameTimer.start();
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
    emit speedChanged(m_speed);
}

void PlaybackTransport::seek(int64_t posMs) {
    QMutexLocker locker(&m_mutex);
    m_currentPos = qMax(int64_t(0), posMs);
    emit posChanged(m_currentPos);
}

void PlaybackTransport::step(int frames) {
    // Assume 30fps for simple stepping logic (33.33ms per frame)
    int64_t stepSize = frames * 33;
    seek(currentPos() + stepSize);
}

void PlaybackTransport::onTick() {
    if (!m_isPlaying) return;

    QMutexLocker locker(&m_mutex);

    // Calculate delta time in real-world ms
    int64_t elapsed = m_frameTimer.restart();

    // Apply speed multiplier to the delta
    m_currentPos += static_cast<int64_t>(elapsed * m_speed);
    // Bounds checking (prevent negative time)
    if (m_currentPos < 0) {
        m_currentPos = 0;
        setPlaying(false);
    }

    emit posChanged(m_currentPos);
}
