#ifndef PLAYBACKTRANSPORT_H
#define PLAYBACKTRANSPORT_H

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <atomic>

#include "playback/framerate.h"

class PlaybackTransport : public QObject {
    Q_OBJECT
    Q_PROPERTY(int64_t currentPos READ currentPos NOTIFY posChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)

public:
    explicit PlaybackTransport(QObject* parent = nullptr);

    int64_t currentPos() const;
    double speed() const;
    bool isPlaying() const;
    int fps() const;
    FrameRate frameRate() const;

    Q_INVOKABLE void setSpeed(double speed);
    Q_INVOKABLE void setPlaying(bool playing);
    Q_INVOKABLE void setFps(int fps);
    Q_INVOKABLE void setFrameRate(int numerator, int denominator);
    Q_INVOKABLE void seek(int64_t posMs);
    Q_INVOKABLE void step(int frames); // e.g., +1 or -1 for frame-stepping

signals:
    void posChanged(int64_t pos);
    void speedChanged(double speed);
    void playingChanged(bool playing);
    void fpsChanged(int fps);
    void frameRateChanged();

private slots:
    void onTick();

private:
    mutable QMutex m_mutex;
    QTimer* m_tickTimer;
    QElapsedTimer m_playStartTime;

    int64_t m_currentPos = 0;
    int64_t m_playStartPos = 0;
    std::atomic<double> m_speed{1.0};
    std::atomic<bool> m_isPlaying{false};
    FrameRate m_frameRate = FrameRate::fromFraction(30, 1);

    const int m_timerIntervalMs = 16; // ~60Hz update rate
};

#endif // PLAYBACKTRANSPORT_H
