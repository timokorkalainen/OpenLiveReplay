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

public:
    explicit ReplayManager(QObject *parent = nullptr);
    ~ReplayManager();

    // Engine Controls used by UIManager
    void startRecording();
    void stopRecording();
    bool isRecording() const { return m_isRecording; }

    // Configuration Setters (invoked by UIManager)
    void setStreamUrls(const QStringList &urls) { m_trackUrls = urls; }
    void setOutputDirectory(const QString &path) { m_outputDir = path; }
    void setBaseFileName(const QString &name) { m_baseFileName = name; }
    void updateTrackUrl(int index, const QString &url);

    // Getters
    QStringList getStreamUrls() const { return m_trackUrls; }
    QString getOutputDirectory() const { return m_outputDir; }
    QString getBaseFileName() const { return m_baseFileName; }

    int64_t getElapsedMs();
signals:
    void masterPulse(int64_t frameIndex, int64_t wallClockUs);

private slots:
    void onTimerTick();

private:
    QString getFullOutputPath();

    bool m_isRecording = false;
    int64_t m_globalFrameCount = 0;

    QStringList m_trackUrls;
    QString m_outputDir;
    QString m_baseFileName;

    QTimer* m_heartbeat;
    Muxer* m_muxer;
    RecordingClock* m_clock;
    QList<StreamWorker*> m_workers;
};

#endif // REPLAYMANAGER_H
