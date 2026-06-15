#ifndef UIMANAGER_H
#define UIMANAGER_H

#include <QObject>
#include <QString>
#include <QUrl>
#include <QHash>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include "settingsmanager.h"
#include "recorder_engine/replaymanager.h"
#include "playback/frameprovider.h"
#include "playback/playbackworker.h"
#include "playback/playbacktransport.h"
#include "playback/audioplayer.h"
#include "midi/midimanager.h"
#include "streamdeck/streamdeckmanager.h"
#include "streamdeck/streamdeckmappingstore.h"

class QScreen;

class UIManager : public QObject {
    Q_OBJECT
    // These allow QML to bind to your settings automatically
    Q_PROPERTY(QStringList streamUrls READ streamUrls WRITE setStreamUrls NOTIFY streamUrlsChanged)
    Q_PROPERTY(QStringList streamNames READ streamNames WRITE setStreamNames NOTIFY streamNamesChanged)
    Q_PROPERTY(QStringList streamIds READ streamIds WRITE setStreamIds NOTIFY streamIdsChanged)
    Q_PROPERTY(QString saveLocation READ saveLocation WRITE setSaveLocation NOTIFY saveLocationChanged)
    Q_PROPERTY(QString fileName READ fileName WRITE setFileName NOTIFY fileNameChanged)
    Q_PROPERTY(int recordWidth READ recordWidth WRITE setRecordWidth NOTIFY recordWidthChanged)
    Q_PROPERTY(int recordHeight READ recordHeight WRITE setRecordHeight NOTIFY recordHeightChanged)
    Q_PROPERTY(int recordFps READ recordFps WRITE setRecordFps NOTIFY recordFpsChanged)
    Q_PROPERTY(int multiviewCount READ multiviewCount WRITE setMultiviewCount NOTIFY multiviewCountChanged)
    Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordingStatusChanged)
    Q_PROPERTY(QVariantList playbackProviders READ playbackProviders NOTIFY playbackProvidersChanged)
    Q_PROPERTY(int64_t recordedDurationMs READ recordedDurationMs NOTIFY recordedDurationMsChanged)
    Q_PROPERTY(int64_t scrubPosition READ scrubPosition NOTIFY scrubPositionChanged)
    Q_PROPERTY(QString playbackTimecode READ playbackTimecode NOTIFY playbackTimecodeChanged)
    Q_PROPERTY(qint64 recordingStartEpochMs READ recordingStartEpochMs NOTIFY recordingStartEpochMsChanged)
    Q_PROPERTY(bool timeOfDayMode READ timeOfDayMode WRITE setTimeOfDayMode NOTIFY timeOfDayModeChanged)
    Q_PROPERTY(int liveBufferMs READ liveBufferMs CONSTANT)
    Q_PROPERTY(QStringList midiPorts READ midiPorts NOTIFY midiPortsChanged)
    Q_PROPERTY(int midiPortIndex READ midiPortIndex WRITE setMidiPortIndex NOTIFY midiPortIndexChanged)
    Q_PROPERTY(bool midiConnected READ midiConnected NOTIFY midiConnectedChanged)
    Q_PROPERTY(int midiLearnAction READ midiLearnAction NOTIFY midiLearnActionChanged)
    Q_PROPERTY(int midiLearnMode READ midiLearnMode NOTIFY midiLearnActionChanged)
    Q_PROPERTY(QString midiPortName READ midiPortName NOTIFY midiPortNameChanged)
    Q_PROPERTY(int midiBindingsVersion READ midiBindingsVersion NOTIFY midiBindingsChanged)
    Q_PROPERTY(int midiLastValuesVersion READ midiLastValuesVersion NOTIFY midiLastValuesChanged)
    Q_PROPERTY(PlaybackTransport* transport READ transport CONSTANT)
    Q_PROPERTY(StreamDeckManager* streamDeck READ streamDeck CONSTANT)
    Q_PROPERTY(int streamDeckLearnAction READ streamDeckLearnAction NOTIFY streamDeckLearnActionChanged)
    Q_PROPERTY(int streamDeckBindingsVersion READ streamDeckBindingsVersion NOTIFY streamDeckBindingsChanged)
    Q_PROPERTY(QVariantList screenOptions READ screenOptions NOTIFY screensChanged)
    Q_PROPERTY(bool screensReady READ screensReady NOTIFY screensChanged)
    Q_PROPERTY(int screenCount READ screenCount NOTIFY screensChanged)
    Q_PROPERTY(QVariantList viewSlotMap READ viewSlotMap NOTIFY viewSlotMapChanged)
    Q_PROPERTY(int sourceEnabledVersion READ sourceEnabledVersion NOTIFY sourceEnabledChanged)
    Q_PROPERTY(bool followLive READ followLive NOTIFY followLiveChanged)
    // Bumped whenever any source's live connection state changes; QML reads
    // it to re-evaluate isSourceConnected() bindings.
    Q_PROPERTY(
        int sourceConnectionVersion READ sourceConnectionVersion NOTIFY sourceConnectionChanged)
    // Bumped when any source's trim changes (config load / programmatic set) so
    // QML re-reads sourceTrimOffset() bindings.
    Q_PROPERTY(int sourceTrimVersion READ sourceTrimVersion NOTIFY sourceTrimChanged)

public:
    explicit UIManager(ReplayManager *engine, QObject *parent = nullptr);
    ~UIManager() override;

    // Getters for QML
    QStringList streamUrls() const;
    QStringList streamNames() const;
    QStringList streamIds() const;
    QString saveLocation() const;
    QString fileName() const;
    int recordWidth() const;
    int recordHeight() const;
    int recordFps() const;
    int multiviewCount() const;
    bool isRecording() const;
    QVariantList playbackProviders() const;
    int64_t recordedDurationMs();
    int64_t scrubPosition();
    // The exact timecode the playback UI shows — the single source of truth for
    // both the on-screen label and the Stream Deck (time-of-day aware; HH:MM:SS.FF
    // from scrubPosition otherwise). The deck must never compute its own.
    QString playbackTimecode();
    qint64 recordingStartEpochMs() const;
    bool timeOfDayMode() const;
    int liveBufferMs() const;
    QStringList midiPorts() const;
    int midiPortIndex() const;
    bool midiConnected() const;
    int midiLearnAction() const;
    int midiLearnMode() const { return m_midiLearnMode; }
    QString midiPortName() const;
    int midiBindingsVersion() const;
    int midiLastValuesVersion() const;
    PlaybackTransport* transport() const { return m_transport; }
    StreamDeckManager* streamDeck() const { return m_streamDeckManager; }
    int streamDeckLearnAction() const { return m_streamDeckLearnAction; }
    int streamDeckBindingsVersion() const { return m_streamDeckBindingsVersion; }
    QVariantList screenOptions() const;
    bool screensReady() const;
    int screenCount() const;
    QVariantList viewSlotMap() const;
    int sourceEnabledVersion() const { return m_sourceEnabledVersion; }
    bool followLive() const { return m_followLive; }
    int sourceConnectionVersion() const { return m_sourceConnectionVersion; }
    int sourceTrimVersion() const { return m_sourceTrimVersion; }

    // Setters
    void setStreamUrls(const QStringList &urls);
    void setStreamNames(const QStringList &names);
    void setStreamIds(const QStringList &ids);
    void setSaveLocation(const QString &path);
    void setFileName(const QString &name);
    void setRecordWidth(int width);
    void setRecordHeight(int height);
    void setRecordFps(int fps);
    void setMultiviewCount(int count);
    void setTimeOfDayMode(bool enabled);

    void refreshProviders();

    // Logic
    Q_INVOKABLE void openStreams();
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void updateUrl(int index, const QString &url);
    Q_INVOKABLE void updateStreamName(int index, const QString &name);
    Q_INVOKABLE void updateStreamId(int index, const QString &id);
    Q_INVOKABLE QString sourceDisplayLabel(int sourceIndex) const;
    Q_INVOKABLE QVariantList metadataFieldDefinitions() const;
    Q_INVOKABLE void setMetadataFieldDefinitions(const QVariantList &fields);
    Q_INVOKABLE QVariantList sourceMetadataItems(int index) const;
    Q_INVOKABLE void setSourceMetadataItems(int index, const QVariantList &items);
    Q_INVOKABLE void loadSettings();
    Q_INVOKABLE void addStream();             // Increases stream count
    Q_INVOKABLE void removeStream(int index); // (Optional) for better UX
    Q_INVOKABLE void saveSettings();          // Manual save trigger
    Q_INVOKABLE void setSaveLocationFromUrl(const QUrl &folderUrl);
    Q_INVOKABLE void scrubToLive();
    Q_INVOKABLE void captureSnapshot(bool singleView, int selectedIndex, int64_t playheadMs);
    Q_INVOKABLE void refreshMidiPorts();
    Q_INVOKABLE void setMidiPortIndex(int index);
    Q_INVOKABLE void beginMidiLearn(int action);
    Q_INVOKABLE void beginMidiLearnJogForward(int action);
    Q_INVOKABLE void beginMidiLearnJogBackward(int action);
    Q_INVOKABLE void clearMidiBinding(int action);
    Q_INVOKABLE QString midiBindingLabel(int action) const;
    Q_INVOKABLE void beginStreamDeckLearn(int action);
    Q_INVOKABLE void clearStreamDeckBinding(int action);
    Q_INVOKABLE void resetStreamDeckDefaults();
    Q_INVOKABLE QString streamDeckBindingLabel(int action) const;
    Q_INVOKABLE int midiLastValue(int action) const;
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void rewind5x();
    Q_INVOKABLE void forward5x();
    Q_INVOKABLE void stepFrame();
    Q_INVOKABLE void stepFrameBack();
    Q_INVOKABLE void goLive();
    Q_INVOKABLE void captureCurrent();
    Q_INVOKABLE void requestNewWindowScene();
    Q_INVOKABLE void setPlaybackViewState(bool singleView, int selectedIndex);
    Q_INVOKABLE void cancelFollowLive();

    Q_INVOKABLE void refreshScreens();
    Q_INVOKABLE QScreen* screenAt(int index) const;
    Q_INVOKABLE void toggleSourceEnabled(int sourceIndex);
    Q_INVOKABLE bool isSourceEnabled(int sourceIndex) const;
    // True only while recording and the source's worker reports a live feed.
    Q_INVOKABLE bool isSourceConnected(int sourceIndex) const;
    // Config-time check: another source carries the same non-empty URL.
    // Surfaces the duplicate-stream misconfiguration that two workers
    // pulling one URL otherwise hides.
    Q_INVOKABLE bool hasDuplicateUrl(int sourceIndex) const;
    Q_INVOKABLE int sourceTrimOffset(int sourceIndex) const;
    Q_INVOKABLE void setSourceTrimOffset(int sourceIndex, int ms);

    //Playback
    Q_INVOKABLE void seekPlayback(int64_t ms);


    QString getSettingsPath(QString fileName);
signals:
    void streamUrlsChanged();
    void streamNamesChanged();
    void streamIdsChanged();
    void saveLocationChanged();
    void fileNameChanged();
    void recordWidthChanged();
    void recordHeightChanged();
    void recordFpsChanged();
    void multiviewCountChanged();
    void recordingStatusChanged();
    void playbackProvidersChanged();
    void recordingStarted();
    void recordingStopped();
    void recordingFailed(const QString& reason);
    void recordedDurationMsChanged();
    void scrubPositionChanged();
    void playbackTimecodeChanged();
    void recordingStartEpochMsChanged();
    void timeOfDayModeChanged();
    void midiPortsChanged();
    void midiPortIndexChanged();
    void midiConnectedChanged();
    void midiLearnActionChanged();
    void midiBindingsChanged();
    void midiLastValuesChanged();
    void midiPortNameChanged();
    void feedSelectRequested(int index);
    void multiviewRequested();
    void screensChanged();
    void viewSlotMapChanged();
    void sourceEnabledChanged();
    void followLiveChanged();
    void streamDeckLearnActionChanged();
    void streamDeckBindingsChanged();
    void sourceConnectionChanged();
    void sourceTrimChanged();

public slots:
    // Called when the user clicks "Record" in the UI
    void onStartRequested();

    // Called when the user clicks "Stop"
    void onStopRequested();

    // Called when a URL is changed in the UI text fields
    void updateStreamUrl(int index, const QString& url);

    // Receives ReplayManager::masterPulse(frameIndex, elapsedMs).
    // NOTE: parameter order is (frame index, elapsed ms) — they were
    // previously named in the opposite order, which was a landmine.
    void onRecorderPulse(int64_t frameIndex, int64_t elapsedMs);

    // Receives ReplayManager::sourceConnectionChanged on the main thread.
    void onSourceConnectionChanged(int sourceIndex, bool connected);

private:
    void syncActiveStreams();
    int activeViewCount() const;
    QStringList activeStreamUrls() const;
    QStringList activeStreamNames() const;
    void rebuildSlotMap();
    void ensureSourceEnabledSize();
    void updateXTouchLcd();
    void updateXTouchDisplay();

    void restartPlaybackWorker();

    // Shared control-action dispatch used by both MIDI bindings and the
    // Stream Deck. Action ids documented in streamdeck/streamdeckmanager.h.
    void dispatchControlAction(int action, bool isRelease);
    void jogStep(int delta);
    void setFollowLive(bool on);
    void pushStreamDeckMaps();
    void pushDeckTimecode();
    void shuttleStep(int delta);

    ReplayManager* m_replayManager;
    AppSettings m_currentSettings;
    SettingsManager* m_settingsManager;
    QString m_configPath;
    PlaybackWorker* m_playbackWorker = nullptr;
    QList<FrameProvider*> m_providers;
    PlaybackTransport *m_transport;
    AudioPlayer *m_audioPlayer = nullptr;
    bool m_followLive = false;
    int m_liveBufferMs = 1000;
    MidiManager* m_midiManager = nullptr;
    StreamDeckManager* m_streamDeckManager = nullptr;
    StreamDeckMappingStore m_streamDeckStore;
    int m_streamDeckLearnAction = -1;
    int m_streamDeckBindingsVersion = 0;
    int m_midiLearnAction = -1;
    bool m_playbackSingleView = false;
    int m_playbackSelectedIndex = -1;
    int m_midiBindingsVersion = 0;
    int m_midiLastValuesVersion = 0;
    bool m_holdWasPlaying = false;
    int m_holdAction = -1;
    QElapsedTimer m_xTouchLastSend;
    QString m_xTouchLastText;
    int m_xTouchMinIntervalMs = 25;
    bool m_xTouchTestSent = false;
    QElapsedTimer m_jogTimer;

    struct MidiBinding {
        int status = -1;
        int data1 = -1;
        int data2 = -1;
    };
    QHash<int, MidiBinding> m_midiBindings;
    QHash<int, int> m_midiLastValues;
    QHash<int, int> m_midiBindingData2Forward;
    QHash<int, int> m_midiBindingData2Backward;

    QList<bool> m_sourceEnabled;
    QList<int> m_viewSlotMap;       // viewSlotMap[viewIndex] = sourceIndex or -1
    int m_sourceEnabledVersion = 0;

    // Live connection state per source index, mirrored from the workers via
    // ReplayManager::sourceConnectionChanged. Reset to all-false on
    // start/stop so a stale "connected" never lingers across sessions.
    QList<bool> m_sourceConnected;
    int m_sourceConnectionVersion = 0;
    int m_sourceTrimVersion = 0;
    void resetSourceConnection();

    QList<QScreen*> m_screens;
    QVariantList m_screenOptions;

    enum MidiLearnMode {
        LearnControl = 0,
        LearnJogForward = 1,
        LearnJogBackward = 2
    };
    int m_midiLearnMode = LearnControl;
};

#endif // UIMANAGER_H
