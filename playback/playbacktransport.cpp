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
    return m_speed.load(std::memory_order_relaxed);
}

bool PlaybackTransport::isPlaying() const {
    return m_isPlaying.load(std::memory_order_relaxed);
}

int PlaybackTransport::fps() const {
    return m_fps.load(std::memory_order_relaxed);
}

void PlaybackTransport::setPlaying(bool playing) {
    if (m_isPlaying.load() == playing) return;

    {
        QMutexLocker locker(&m_mutex);
        m_isPlaying.store(playing);
        if (playing) {
            m_playStartPos = m_currentPos;
            m_playStartTime.start();
        }
    }
    // QTimer must be driven from its owning (main) thread; setPlaying is
    // only ever called from the UI/main thread.
    if (playing) m_tickTimer->start(); else m_tickTimer->stop();
    emit playingChanged(playing);
}

void PlaybackTransport::setSpeed(double speed) {
    speed = qRound(speed * 100.0) / 100.0;
    if (qFuzzyCompare(m_speed.load(), speed)) return;
    {
        QMutexLocker locker(&m_mutex);
        if (m_isPlaying.load()) {
            // Bank the position progressed at the OLD speed before the
            // new one takes effect.
            m_currentPos = m_playStartPos
                + static_cast<int64_t>(m_playStartTime.elapsed() * m_speed.load());
            m_playStartPos = m_currentPos;
            m_playStartTime.restart();
        }
        m_speed.store(speed);
    }
    emit speedChanged(speed);
}

void PlaybackTransport::setFps(int fps) {
    if (fps <= 0) return;
    if (m_fps.exchange(fps) == fps) return;
    emit fpsChanged(fps);
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
    int fps = qMax(1, m_fps.load());
    int64_t stepSize = static_cast<int64_t>(frames * (1000.0 / fps));
    seek(currentPos() + stepSize);
}

void PlaybackTransport::onTick() {
    if (!m_isPlaying.load()) return;

    int64_t newPos = 0;
    bool shouldStop = false;
    {
        QMutexLocker locker(&m_mutex);
        // Calculate position from play start to avoid cumulative drift
        int64_t elapsed = m_playStartTime.elapsed();
        m_currentPos = m_playStartPos + static_cast<int64_t>(elapsed * m_speed.load());
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
