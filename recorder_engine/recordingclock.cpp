#include "recordingclock.h"

RecordingClock::RecordingClock(QObject *parent)
    : QObject(parent)
{
    // Timer is invalid until start() is called
}

void RecordingClock::start() {
    QMutexLocker locker(&m_mutex);
    m_timer.start();
}

int64_t RecordingClock::elapsedMs() const {
    // We don't strictly need a mutex for reading QElapsedTimer on macOS,
    // but it's good practice for cross-platform stability.
    QMutexLocker locker(&m_mutex);
    if (!m_timer.isValid()) {
        return 0;
    }
    return m_timer.elapsed();
}

bool RecordingClock::isValid() const {
    QMutexLocker locker(&m_mutex);
    return m_timer.isValid();
}
