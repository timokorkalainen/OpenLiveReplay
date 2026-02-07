#ifndef UIMANAGER_H
#define UIMANAGER_H

#include <QObject>
#include <QString>
#include <QUrl>
#include "settingsmanager.h"
#include "recorder_engine/replaymanager.h"
#include "playback/frameprovider.h"
#include "playback/playbackworker.h"
#include "playback/playbacktransport.h"

class UIManager : public QObject {
    Q_OBJECT
    // These allow QML to bind to your settings automatically
    Q_PROPERTY(QStringList streamUrls READ streamUrls WRITE setStreamUrls NOTIFY streamUrlsChanged)
    Q_PROPERTY(QStringList streamNames READ streamNames WRITE setStreamNames NOTIFY streamNamesChanged)
    Q_PROPERTY(QString saveLocation READ saveLocation WRITE setSaveLocation NOTIFY saveLocationChanged)
    Q_PROPERTY(QString fileName READ fileName WRITE setFileName NOTIFY fileNameChanged)
    Q_PROPERTY(int recordWidth READ recordWidth WRITE setRecordWidth NOTIFY recordWidthChanged)
    Q_PROPERTY(int recordHeight READ recordHeight WRITE setRecordHeight NOTIFY recordHeightChanged)
    Q_PROPERTY(int recordFps READ recordFps WRITE setRecordFps NOTIFY recordFpsChanged)
    Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordingStatusChanged)
    Q_PROPERTY(QVariantList playbackProviders READ playbackProviders NOTIFY playbackProvidersChanged)
    Q_PROPERTY(int64_t recordedDurationMs READ recordedDurationMs NOTIFY recordedDurationMsChanged)
    Q_PROPERTY(int64_t scrubPosition READ scrubPosition NOTIFY scrubPositionChanged)
    Q_PROPERTY(qint64 recordingStartEpochMs READ recordingStartEpochMs NOTIFY recordingStartEpochMsChanged)
    Q_PROPERTY(bool timeOfDayMode READ timeOfDayMode WRITE setTimeOfDayMode NOTIFY timeOfDayModeChanged)
    Q_PROPERTY(PlaybackTransport* transport READ transport CONSTANT)

public:
    explicit UIManager(ReplayManager *engine, QObject *parent = nullptr);

    // Getters for QML
    QStringList streamUrls() const;
    QStringList streamNames() const;
    QString saveLocation() const;
    QString fileName() const;
    int recordWidth() const;
    int recordHeight() const;
    int recordFps() const;
    bool isRecording() const;
    QVariantList playbackProviders() const;
    int64_t recordedDurationMs();
    int64_t scrubPosition();
    qint64 recordingStartEpochMs() const;
    bool timeOfDayMode() const;
    PlaybackTransport* transport() const { return m_transport; }

    // Setters
    void setStreamUrls(const QStringList &urls);
    void setStreamNames(const QStringList &names);
    void setSaveLocation(const QString &path);
    void setFileName(const QString &name);
    void setRecordWidth(int width);
    void setRecordHeight(int height);
    void setRecordFps(int fps);
    void setTimeOfDayMode(bool enabled);

    void refreshProviders();

    // Logic
    Q_INVOKABLE void openStreams();
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void updateUrl(int index, const QString &url);
    Q_INVOKABLE void updateStreamName(int index, const QString &name);
    Q_INVOKABLE void loadSettings();
    Q_INVOKABLE void addStream();             // Increases stream count
    Q_INVOKABLE void removeStream(int index); // (Optional) for better UX
    Q_INVOKABLE void saveSettings();          // Manual save trigger
    Q_INVOKABLE void setSaveLocationFromUrl(const QUrl &folderUrl);
    Q_INVOKABLE void scrubToLive();
    Q_INVOKABLE void captureSnapshot(bool singleView, int selectedIndex, int64_t playheadMs);

    //Playback
    Q_INVOKABLE void seekPlayback(int64_t ms);


    QString getSettingsPath(QString fileName);
signals:
    void streamUrlsChanged();
    void streamNamesChanged();
    void saveLocationChanged();
    void fileNameChanged();
    void recordWidthChanged();
    void recordHeightChanged();
    void recordFpsChanged();
    void recordingStatusChanged();
    void playbackProvidersChanged();
    void recordingStarted();
    void recordingStopped();
    void recordedDurationMsChanged();
    void scrubPositionChanged();
    void recordingStartEpochMsChanged();
    void timeOfDayModeChanged();

public slots:
    // Called when the user clicks "Record" in the UI
    void onStartRequested();

    // Called when the user clicks "Stop"
    void onStopRequested();

    // Called when a URL is changed in the UI text fields
    void updateStreamUrl(int index, const QString& url);

    void onRecorderPulse(int64_t elapsed, int64_t frameCount);

private:
    void restartPlaybackWorker();

    ReplayManager* m_replayManager;
    AppSettings m_currentSettings;
    SettingsManager* m_settingsManager;
    QString m_configPath;
    PlaybackWorker* m_playbackWorker = nullptr;
    QList<FrameProvider*> m_providers;
    PlaybackTransport *m_transport;
    bool m_followLive = false;
    int m_liveBufferMs = 200;
};

#endif // UIMANAGER_H
