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

    // Source configuration (N sources)
    void setSourceUrls(const QStringList &urls) { m_sourceUrls = urls; }
    void setSourceNames(const QStringList &names) { m_sourceNames = names; }
    QStringList getSourceUrls() const { return m_sourceUrls; }
    QStringList getSourceNames() const { return m_sourceNames; }

    // View configuration (M views/tracks)
    void setViewCount(int count) { m_viewCount = count; }
    int viewCount() const { return m_viewCount; }

    // View mapping: viewSlotMap[v] = sourceIndex (-1 = unmapped/blue)
    // This is the ONLY thing that changes during toggle — zero FFmpeg impact.
    void updateViewMapping(const QList<int>& viewSlotMap);

    // For user editing a source URL during recording (real FFmpeg reconnect)
    void updateSourceUrl(int sourceIndex, const QString &url);

    // Stream names for view tracks in the muxer
    void setViewNames(const QStringList &names) { m_viewNames = names; }

    // Other configuration
    void setOutputDirectory(const QString &path) { m_outputDir = path; }
    void setBaseFileName(const QString &name) { m_baseFileName = name; }
    void setVideoWidth(int width) { m_videoWidth = width; }
    void setVideoHeight(int height) { m_videoHeight = height; }
    void setFps(int fps) { m_fps = fps; }

    // Getters
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
    void writeBlueFrames();

    bool m_isRecording = false;
    int64_t m_globalFrameCount = 0;

    // Source config
    QStringList m_sourceUrls;
    QStringList m_sourceNames;

    // View config
    int m_viewCount = 4;
    QStringList m_viewNames;
    QList<int> m_viewSlotMap;       // viewSlotMap[v] = source index or -1

    QString m_outputDir;
    QString m_baseFileName;
    QString m_sessionFileName;

    int m_videoWidth = 1920;
    int m_videoHeight = 1080;
    int m_fps = 30;

    QTimer* m_heartbeat;
    Muxer* m_muxer;
    RecordingClock* m_clock;
    QList<StreamWorker*> m_workers;  // One per SOURCE (not per view)
    qint64 m_recordingStartEpochMs = 0;

    // Blue frame encoder for unmapped views
    AVCodecContext* m_blueEncCtx = nullptr;
    AVFrame* m_blueFrame = nullptr;
    bool setupBlueEncoder();
    void cleanupBlueEncoder();
};

#endif // REPLAYMANAGER_H
