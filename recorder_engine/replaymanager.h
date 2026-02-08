#ifndef REPLAYMANAGER_H
#define REPLAYMANAGER_H

#include <QObject>
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
    void setStreamNames(const QStringList &names) { m_streamNames = names; }
    void setOutputDirectory(const QString &path) { m_outputDir = path; }
    void setBaseFileName(const QString &name) { m_baseFileName = name; }
    void setVideoWidth(int width) { m_videoWidth = width; }
    void setVideoHeight(int height) { m_videoHeight = height; }
    void setFps(int fps) { m_fps = fps; }
    void updateTrackUrl(int index, const QString &url);

    // Getters
    QStringList getStreamUrls() const { return m_trackUrls; }
    QStringList getStreamNames() const { return m_streamNames; }
    QString getOutputDirectory() const { return m_outputDir; }
    QString getBaseFileName() const { return m_baseFileName; }
    int getVideoWidth() const { return m_videoWidth; }
    int getVideoHeight() const { return m_videoHeight; }
    int getFps() const { return m_fps; }

    int64_t getElapsedMs();
    QString getVideoPath();
    qint64 getRecordingStartEpochMs() const { return m_recordingStartEpochMs; }
signals:
    void masterPulse(int64_t frameIndex, int64_t wallClockUs);

private slots:
    void onTimerTick();

private:
    QString getFullOutputPath();

    bool m_isRecording = false;
    int64_t m_globalFrameCount = 0;

    QStringList m_trackUrls;
    QStringList m_streamNames;
    QString m_outputDir;
    QString m_baseFileName;
    QString m_sessionFileName;

    int m_videoWidth = 1920;
    int m_videoHeight = 1080;
    int m_fps = 30;

    QTimer* m_heartbeat;
    Muxer* m_muxer;
    RecordingClock* m_clock;
    QList<StreamWorker*> m_workers;
    qint64 m_recordingStartEpochMs = 0;
};

#endif // REPLAYMANAGER_H
