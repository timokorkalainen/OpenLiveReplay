#ifndef RECORDINGCLOCK_H
#define RECORDINGCLOCK_H

#include <QObject>
#include <QElapsedTimer>
#include <QMutex>

class RecordingClock : public QObject {
    Q_OBJECT
public:
    explicit RecordingClock(QObject *parent = nullptr);

    // Starts or resets the "Time Zero" for the session
    void start();

    // Returns milliseconds since start() was called
    // Thread-safe: can be called by multiple workers simultaneously
    int64_t elapsedMs() const;

    // Checks if the clock has been started
    bool isValid() const;

private:
    mutable QMutex m_mutex;
    QElapsedTimer m_timer;
};

#endif // RECORDINGCLOCK_H
