#ifndef REPLAYMANAGER_H
#define REPLAYMANAGER_H

#include <QObject>
#include <QMap>
#include <QString>
#include "recordingclock.h"
#include "muxer.h"
#include "streamworker.h"

class ReplayManager : public QObject
{
    Q_OBJECT
    // Define properties if you want to track status in QML
    Q_PROPERTY(bool isRecording READ isRecording NOTIFY isRecordingChanged)
    Q_PROPERTY(int trackCount READ trackCount WRITE setTrackCount NOTIFY trackCountChanged)
    Q_PROPERTY(QStringList trackUrls READ trackUrls NOTIFY trackUrlsChanged)

public:
    explicit ReplayManager(QObject *parent = nullptr);
    ~ReplayManager();

    // Property Accessors
    int trackCount() const;
    QStringList trackUrls() const;
    bool isRecording() const;

    // QML Invokable Methods
    Q_INVOKABLE void setTrackCount(int count);
    Q_INVOKABLE void setTrackUrl(int index, const QString &url);
    Q_INVOKABLE void startRecording(const QString &path);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void applyTrackSource(int index, const QString &url);

    int64_t currentStreamTimeMs();
signals:
    void trackCountChanged();
    void trackUrlsChanged();
    void isRecordingChanged();
    void errorOccurred(QString message);
    void masterPulse(int64_t frameIndex, int64_t wallClockUs);

private slots:
    void onTimerTick();

private:
    QTimer* m_heartbeat;
    int64_t m_globalFrameCount = 0;
    RecordingClock* m_clock;

    Muxer *m_muxer;
    bool m_isRecording = false;
    QStringList m_trackUrls;
    QMap<int, StreamWorker*> m_workers;

    void startWorker(int index, const QString &url);
};

#endif // REPLAYMANAGER_H
