#include "playbacktransport.h"

#include <QtGlobal>

namespace {
qint64 firstFrameIndexAtOrAfterMs(FrameRate rate, qint64 ms) {
    if (!rate.isValid() || ms <= 0) return 0;

    qint64 index = rate.msToFrameIndex(ms);
    while (rate.frameIndexToMs(index) < ms)
        ++index;
    while (index > 0 && rate.frameIndexToMs(index - 1) >= ms)
        --index;
    return index;
}

qint64 firstFrameIndexAfterMs(FrameRate rate, qint64 ms) {
    qint64 index = firstFrameIndexAtOrAfterMs(rate, ms);
    while (rate.frameIndexToMs(index) <= ms)
        ++index;
    return index;
}
} // namespace

PlaybackTransport::PlaybackTransport(QObject* parent)
    : QObject(parent), m_tickTimer(new QTimer(this)) {
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
    return frameRate().roundedFps();
}

FrameRate PlaybackTransport::frameRate() const {
    QMutexLocker locker(&m_mutex);
    return m_frameRate;
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
    if (playing)
        m_tickTimer->start();
    else
        m_tickTimer->stop();
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
            m_currentPos =
                m_playStartPos + static_cast<int64_t>(m_playStartTime.elapsed() * m_speed.load());
            m_playStartPos = m_currentPos;
            m_playStartTime.restart();
        }
        m_speed.store(speed);
    }
    emit speedChanged(speed);
}

void PlaybackTransport::setFps(int fps) {
    setFrameRate(fps, 1);
}

void PlaybackTransport::setFrameRate(int numerator, int denominator) {
    FrameRate next = FrameRate::fromFraction(numerator, denominator);
    if (!next.isValid()) return;

    int previousRounded = 30;
    const int nextRounded = next.roundedFps();
    {
        QMutexLocker locker(&m_mutex);
        if (m_frameRate.numerator == next.numerator &&
            m_frameRate.denominator == next.denominator) {
            return;
        }
        previousRounded = m_frameRate.roundedFps();
        m_frameRate = next;
    }

    if (previousRounded != nextRounded) emit fpsChanged(nextRounded);
    emit frameRateChanged();
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
    if (frames == 0) return;
    const FrameRate rate = frameRate();
    if (!rate.isValid()) return;

    const qint64 pos = currentPos();
    const qint64 targetFrame =
        frames > 0 ? firstFrameIndexAfterMs(rate, pos) + qint64(frames - 1)
                   : qMax<qint64>(0, firstFrameIndexAtOrAfterMs(rate, pos) + qint64(frames));
    seek(rate.frameIndexToMs(targetFrame));
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
