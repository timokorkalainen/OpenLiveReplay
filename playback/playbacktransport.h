#ifndef PLAYBACKTRANSPORT_H
#define PLAYBACKTRANSPORT_H

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>

class PlaybackTransport : public QObject {
    Q_OBJECT
    Q_PROPERTY(int64_t currentPos READ currentPos NOTIFY posChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)

public:
    explicit PlaybackTransport(QObject *parent = nullptr);

    int64_t currentPos() const;
    double speed() const;
    bool isPlaying() const;

    Q_INVOKABLE void setSpeed(double speed);
    Q_INVOKABLE void setPlaying(bool playing);
    Q_INVOKABLE void seek(int64_t posMs);
    Q_INVOKABLE void step(int frames); // e.g., +1 or -1 for frame-stepping

signals:
    void posChanged(int64_t pos);
    void speedChanged(double speed);
    void playingChanged(bool playing);

private slots:
    void onTick();

private:
    mutable QMutex m_mutex;
    QTimer *m_tickTimer;
    QElapsedTimer m_frameTimer;
    QElapsedTimer m_playStartTime;

    int64_t m_currentPos = 0;
    int64_t m_playStartPos = 0;
    double m_speed = 1.0;
    bool m_isPlaying = false;

    const int m_timerIntervalMs = 16; // ~60Hz update rate
};

#endif // PLAYBACKTRANSPORT_H
