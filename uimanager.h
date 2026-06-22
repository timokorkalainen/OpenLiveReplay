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
#include <QMap>
#include <vector>
#include "settingsmanager.h"
#include "project/projectsettingsimporter.h"
#include "recorder_engine/replaymanager.h"
#include "recorder_engine/benchmark/runcodecbenchmark.h"
#include "recorder_engine/codec/videocodecchoice.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#include <atomic>
#include "recorder_engine/ingest/ingestsession.h"
#include "playback/frameprovider.h"
#include "playback/playbackworker.h"
#include "playback/playbacktransport.h"
#include "playback/audioplayer.h"
#include "playback/seekcoalescer.h"
#include "playback/replayplaylist.h"
#include "playback/playlistplayout.h"
#include "playback/telemetrytimelinereader.h"
#include "midi/midimanager.h"
#include "streamdeck/streamdeckmanager.h"
#include "streamdeck/streamdeckmappingstore.h"

class QScreen;
class ProjectImportClient;
class TelemetryClient;

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
    Q_PROPERTY(QString recordCodec READ recordCodec WRITE setRecordCodec NOTIFY recordCodecChanged)
    Q_PROPERTY(bool h264EncodeAvailable READ h264EncodeAvailable NOTIFY h264EncodeAvailableChanged)
    Q_PROPERTY(bool benchmarkRunning READ benchmarkRunning NOTIFY benchmarkRunningChanged)
    Q_PROPERTY(QVariantMap benchmarkResult READ benchmarkResult NOTIFY benchmarkResultChanged)
    Q_PROPERTY(int audioOutputLatencyMs READ audioOutputLatencyMs WRITE setAudioOutputLatencyMs
                   NOTIFY audioOutputLatencyChanged)
    Q_PROPERTY(int recordFps READ recordFps WRITE setRecordFps NOTIFY recordFpsChanged)
    Q_PROPERTY(int recordFpsNumerator READ recordFpsNumerator NOTIFY recordFpsChanged)
    Q_PROPERTY(int recordFpsDenominator READ recordFpsDenominator NOTIFY recordFpsChanged)
    Q_PROPERTY(int multiviewCount READ multiviewCount WRITE setMultiviewCount NOTIFY multiviewCountChanged)
    Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordingStatusChanged)
    Q_PROPERTY(QVariantList playbackProviders READ playbackProviders NOTIFY playbackProvidersChanged)
    Q_PROPERTY(FrameProvider* multiviewPreviewProvider READ multiviewPreviewProvider NOTIFY
                   playbackProvidersChanged)
    Q_PROPERTY(
        FrameProvider* pgmPreviewProvider READ pgmPreviewProvider NOTIFY playbackProvidersChanged)
    Q_PROPERTY(int64_t recordedDurationMs READ recordedDurationMs NOTIFY recordedDurationMsChanged)
    Q_PROPERTY(int64_t scrubPosition READ scrubPosition NOTIFY scrubPositionChanged)
    Q_PROPERTY(QString playbackTimecode READ playbackTimecode NOTIFY playbackTimecodeChanged)
    Q_PROPERTY(bool playbackSingleView READ playbackSingleView NOTIFY playbackViewStateChanged)
    Q_PROPERTY(int playbackSelectedIndex READ playbackSelectedIndex NOTIFY playbackViewStateChanged)
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
    // Bumped on every per-source SRT stats update so QML re-evaluates the dot
    // color (sourceLinkHealth) and tooltip (sourceStatsTooltip) bindings.
    Q_PROPERTY(int sourceStatsVersion READ sourceStatsVersion NOTIFY sourceStatsChanged)
    // Bumped when the PGM view switches between multiview/single-view or changes
    // the selected source, so QML re-evaluates on-air tally bindings.
    Q_PROPERTY(int playbackViewStateVersion READ playbackViewStateVersion NOTIFY
                   playbackViewStateVersionChanged)
    // Bumped when any source's trim changes (config load / programmatic set) so
    // QML re-reads sourceTrimOffset() bindings.
    Q_PROPERTY(int sourceTrimVersion READ sourceTrimVersion NOTIFY sourceTrimChanged)
    Q_PROPERTY(QString importSettingsUrl READ importSettingsUrl WRITE setImportSettingsUrl NOTIFY importSettingsUrlChanged)
    Q_PROPERTY(QString importPreviewError READ importPreviewError NOTIFY importPreviewChanged)
    Q_PROPERTY(QVariantMap importPreview READ importPreview NOTIFY importPreviewChanged)
    Q_PROPERTY(bool importPreviewReady READ importPreviewReady NOTIFY importPreviewChanged)
    Q_PROPERTY(QString telemetrySseUrl READ telemetrySseUrl NOTIFY telemetryConfigChanged)
    Q_PROPERTY(int telemetryVersion READ telemetryVersion NOTIFY telemetryChanged)
    Q_PROPERTY(
        int broadcastOutputsVersion READ broadcastOutputsVersion NOTIFY broadcastOutputsChanged)
    Q_PROPERTY(int broadcastOutputStatusVersion READ broadcastOutputStatusVersion NOTIFY
                   broadcastOutputStatusChanged)
    // Phase 5: the session timing-reference tier (0=LocalMonotonic, 1=RecoveredConsensus,
    // 2=Ptp), surfaced so the session status surface can show which timebase is
    // authoritative. Defaults to LocalMonotonic — byte-identical when PTP is off.
    Q_PROPERTY(int sessionReferenceTier READ sessionReferenceTier NOTIFY sessionReferenceChanged)

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
    QString recordCodec() const;
    bool h264EncodeAvailable() const { return m_h264EncodeAvailable; }
    bool benchmarkRunning() const { return m_benchmarkRunning; }
    QVariantMap benchmarkResult() const { return m_benchmarkResult; }
    int recordFps() const;
    int recordFpsNumerator() const;
    int recordFpsDenominator() const;
    int audioOutputLatencyMs() const;
    void setAudioOutputLatencyMs(int ms);
    int multiviewCount() const;
    bool isRecording() const;
    QVariantList playbackProviders() const;
    FrameProvider* multiviewPreviewProvider() const { return m_multiviewPreviewProvider; }
    FrameProvider* pgmPreviewProvider() const { return m_pgmPreviewProvider; }
    int64_t recordedDurationMs();
    int64_t scrubPosition();
    // The exact timecode the playback UI shows — the single source of truth for
    // both the on-screen label and the Stream Deck (time-of-day aware; HH:MM:SS.FF
    // from scrubPosition otherwise). The deck must never compute its own.
    QString playbackTimecode();
    // Format a millisecond timeline position as a SMPTE timecode for the configured
    // record rate, auto-selecting drop-frame for 29.97/59.94 (";FF") vs non-drop
    // (":FF"). The single source of truth for every on-screen/deck timecode so the
    // C++ and QML never drift. QML calls this instead of computing its own.
    Q_INVOKABLE QString recordTimecode(qint64 ms) const;
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
    int sourceStatsVersion() const { return m_sourceStatsVersion; }
    int playbackViewStateVersion() const { return m_playbackViewStateVersion; }
    int sourceTrimVersion() const { return m_sourceTrimVersion; }
    QString importSettingsUrl() const;
    QString importPreviewError() const { return m_importPreviewError; }
    QVariantMap importPreview() const { return m_importPreview; }
    bool importPreviewReady() const { return m_hasPendingImport && m_pendingImport.ok; }
    QString telemetrySseUrl() const { return m_currentSettings.telemetrySseUrl; }
    int telemetryVersion() const { return m_telemetryVersion; }
    int broadcastOutputsVersion() const { return m_broadcastOutputsVersion; }
    int broadcastOutputStatusVersion() const { return m_broadcastOutputStatusVersion; }

    // Setters
    void setStreamUrls(const QStringList &urls);
    void setStreamNames(const QStringList &names);
    void setStreamIds(const QStringList &ids);
    void setSaveLocation(const QString &path);
    void setFileName(const QString &name);
    void setRecordWidth(int width);
    void setRecordHeight(int height);
    void setRecordCodec(const QString& codec);
    void setRecordFps(int fps);
    Q_INVOKABLE void setRecordFrameRate(int numerator, int denominator);
    void setMultiviewCount(int count);
    Q_INVOKABLE void runBenchmark();
    Q_INVOKABLE void cancelBenchmark();
    void setTimeOfDayMode(bool enabled);
    void setImportSettingsUrl(const QString &url);

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
    Q_INVOKABLE bool playbackSingleView() const { return m_playbackSingleView; }
    Q_INVOKABLE int playbackSelectedIndex() const { return m_playbackSelectedIndex; }
    Q_INVOKABLE void dispatchExternalAction(int action, bool pressed);
    Q_INVOKABLE void jogExternal(int delta);
    Q_INVOKABLE void shuttleExternal(int delta);
    Q_INVOKABLE void selectFeedExternal(int index);
    Q_INVOKABLE void cancelFollowLive();

    Q_INVOKABLE void refreshScreens();
    Q_INVOKABLE QScreen* screenAt(int index) const;
    Q_INVOKABLE void toggleSourceEnabled(int sourceIndex);
    Q_INVOKABLE bool isSourceEnabled(int sourceIndex) const;
    // True only while recording and the source's worker reports a live feed.
    Q_INVOKABLE bool isSourceConnected(int sourceIndex) const;
    // Source link health for the connection dot, graded per backend (srtHealth /
    // rtmpHealth). 0=N/A (no stats yet), 1=green, 2=amber (stressed), 3=red.
    Q_INVOKABLE int sourceLinkHealth(int sourceIndex) const;
    // True once this source has produced at least one stats snapshot
    // (native SRT/RTMP only); false for UDP/ffmpeg-SRT sources.
    Q_INVOKABLE bool sourceHasStats(int sourceIndex) const;
    // Preformatted multi-line cumulative figures for the dot's hover tooltip.
    Q_INVOKABLE QString sourceStatsTooltip(int sourceIndex) const;
    // Inter-camera confidence tier (Phase 4) for the QML health surface to badge the
    // source: 0=Approximate, 1=Bounded, 2=FrameAccurate. 0 (Approximate) until the
    // source has reported stats. Refreshed on sourceStatsChanged like the other dot bindings.
    Q_INVOKABLE int sourceConfidenceTier(int sourceIndex) const;
    // Phase 5 session-level timing reference. sessionReferenceTier() relays
    // ReplayManager::referenceTier() (0=LocalMonotonic, 1=RecoveredConsensus, 2=Ptp);
    // sessionReferenceStatus() is the one-line operator string for the session status
    // surface / tooltip — e.g. "timing    PTP (external)" or "timing    local monotonic"
    // — mapping the tier+external state to a label exactly like clockQualityLabel /
    // confidenceTierLabel do for the per-source dot. Both default to local monotonic.
    Q_INVOKABLE int sessionReferenceTier() const;
    Q_INVOKABLE QString sessionReferenceStatus() const;
    // Config-time check: another source carries the same non-empty URL.
    // Surfaces the duplicate-stream misconfiguration that two workers
    // pulling one URL otherwise hides.
    Q_INVOKABLE bool hasDuplicateUrl(int sourceIndex) const;
    Q_INVOKABLE int sourceTrimOffset(int sourceIndex) const;
    Q_INVOKABLE void setSourceTrimOffset(int sourceIndex, int ms);
    Q_INVOKABLE void readImportSettings();
    Q_INVOKABLE void applyImportPreview();
    Q_INVOKABLE QVariantMap telemetryAtPlayhead();
    Q_INVOKABLE QVariantList telemetryRowsAtPlayhead();
    Q_INVOKABLE QVariantList ndiOutputRows() const;
    Q_INVOKABLE QVariantMap ndiOutputStatus(const QString& targetId) const;
    Q_INVOKABLE bool ndiOutputEnabled(const QString& busKind, int feedIndex) const;
    Q_INVOKABLE QString ndiOutputSenderName(const QString& busKind, int feedIndex) const;
    Q_INVOKABLE void setNdiOutputEnabled(const QString& busKind, int feedIndex, bool enabled);
    Q_INVOKABLE void setNdiOutputSenderName(const QString& busKind, int feedIndex,
                                            const QString& senderName);

    //Playback
    Q_INVOKABLE void seekPlayback(int64_t ms);
    Q_INVOKABLE void endScrubGesture();
    // Tier3 replay cue list: capture mark-in/out at the current playhead and
    // recall an entry as a frame-perfect armed cut (pre-rolled, no flash).
    Q_INVOKABLE void markIn();
    Q_INVOKABLE void markOut();
    Q_INVOKABLE void recallEntry(int index);
    Q_INVOKABLE int playlistCount() const;
    // EVS rundown auto-playout: play the playlist from `fromIndex`, auto-advancing
    // across each entry boundary with a frame-perfect armed cut (fire-at-out-point),
    // honoring each entry's speed. After the final entry, normal playback continues
    // forward (the recording is a live, growing file). A manual scrub or recall
    // exits playout (operator override). Returns false if the playlist is empty.
    Q_INVOKABLE bool playPlaylist(int fromIndex = 0);
    Q_INVOKABLE void stopPlaylistPlayout();
    Q_INVOKABLE bool playlistPlayoutActive() const { return m_playout.active(); }

    QString getSettingsPath(QString fileName);
signals:
    void streamUrlsChanged();
    void streamNamesChanged();
    void streamIdsChanged();
    void saveLocationChanged();
    void fileNameChanged();
    void recordWidthChanged();
    void recordHeightChanged();
    void recordCodecChanged();
    void h264EncodeAvailableChanged();
    void benchmarkRunningChanged();
    void benchmarkResultChanged();
    void benchmarkProgress(int concurrency, bool sustained);
    void benchmarkFinished();
    void recordFpsChanged();
    void audioOutputLatencyChanged();
    void multiviewCountChanged();
    void recordingStatusChanged();
    void playbackProvidersChanged();
    void recordingStarted();
    void recordingStopped();
    void recordingFailed(const QString& reason);
    void recordingWarning(const QString& message);
    void recordedDurationMsChanged();
    void scrubPositionChanged();
    void playbackTimecodeChanged();
    void playbackViewStateChanged();
    void playbackViewStateVersionChanged();
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
    void sourceStatsChanged();
    void sourceTrimChanged();
    void metadataFieldsChanged();
    void sourceMetadataChanged();
    void importSettingsUrlChanged();
    void importPreviewChanged();
    void telemetryConfigChanged();
    void telemetryChanged();
    void broadcastOutputsChanged();
    void broadcastOutputStatusChanged();
    // Phase 5: the session timing-reference tier/lock state changed (relayed from
    // ReplayManager::referenceTierChanged) — drives the sessionReferenceTier property
    // + the sessionReferenceStatus() line.
    void sessionReferenceChanged();

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

    // Receives ReplayManager::sourceStatsUpdated on the main thread.
    void onSourceStatsUpdated(int sourceIndex, IngestStats stats);

private:
    QString benchmarkCachePath() const;
    static QVariantMap resultToVariantMap(const CodecBenchmarkResult& r);
    // Recomputes m_benchmarkSafeFeedsForChosen from m_benchmarkResult and the
    // current m_currentSettings.videoCodec. Returns -1 when no result is loaded.
    void updateSafeFeedsForChosen();
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
    ReplayPlaylist m_playlist; // Tier3 cue list (markIn/markOut/recall)
    // EVS rundown auto-playout state. m_playout decides which boundary to arm and
    // when; m_playoutMonitor polls the playhead (onPlayoutTick) to arm boundaries
    // and advance on each cut fire; m_playoutCutBaseline tracks the worker's fired-
    // cut count so a boundary fire is detected as an advance.
    PlaylistPlayout m_playout;
    QTimer m_playoutMonitor;
    int m_playoutCutBaseline = 0;
    void onPlayoutTick();
    QList<FrameProvider*> m_providers;
    FrameProvider* m_multiviewPreviewProvider = nullptr;
    FrameProvider* m_pgmPreviewProvider = nullptr;
    PlaybackTransport *m_transport;
    AudioPlayer *m_audioPlayer = nullptr;
    // Scrub coalescing: seek immediately on the first move of a gesture and on
    // release, but commit only the latest target on a single-shot timer in
    // between. SeekCoalescer holds the pure decision logic (unit-tested).
    void commitPendingScrub();
    SeekCoalescer m_seekCoalescer;
    QTimer m_scrubCoalesceTimer;
    static constexpr int kScrubCoalesceMs = 16; // ~one frame at 60fps
    // Wall-clock lead before an entry's out-point at which the playout boundary cut
    // is armed (PlaylistPlayout scales it by speed into a clip-time distance), and
    // how often the playout monitor polls the playhead. The cut fires frame-perfectly
    // on the output thread, so the poll interval does not affect boundary accuracy.
    static constexpr qint64 kPlayoutArmLeadMs = 1500;
    static constexpr int kPlayoutMonitorMs = 16;
    bool m_followLive = false;
    bool m_h264EncodeAvailable = false;
    bool m_benchmarkRunning = false;
    QVariantMap m_benchmarkResult;
    std::atomic<bool> m_benchmarkCancel{false};
    int m_benchmarkSafeFeedsForChosen = -1;
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
    int m_playbackViewStateVersion = 0;
    int m_sourceTrimVersion = 0;
    // Per-source SRT link-health state, keyed by sourceIndex (parallel to
    // m_sourceConnected). last = most recent snapshot (also shown in the tooltip);
    // seen = false until the first snapshot since (re)connect, so the next snapshot
    // re-baselines after a counter reset; health = cached SourceHealth (0=N/A..3=red).
    struct IngestStatsEntry {
        IngestStats last;
        bool seen = false;
        int health = 0;
    };
    std::vector<IngestStatsEntry> m_sourceStats;
    int m_sourceStatsVersion = 0;
    double m_srtAmberPct = 0.02; // retransmit-rate threshold for Amber
    // Phase 5: cached session timing-reference state, mirrored from
    // ReplayManager::referenceTierChanged so the QML status surface binds without
    // calling into the engine on every paint. Defaults to the local monotonic tier.
    int m_sessionReferenceTier = 0; // 0=LocalMonotonic (ReferenceTier)
    bool m_sessionReferenceExternal = false;
    void resetSourceStats(int count);
    void resetSourceConnection();
    void updateReplayTelemetryFeeds();
    void clearImportPreview();
    void applyBroadcastOutputs(const QList<OutputTargetAssignment>& outputs);
    bool loadTelemetryTimeline(const QString &filePath, bool notify = true);
    QVariantMap recordingTelemetryStateAt(qint64 playheadMs) const;

    struct TelemetryTimelineEntry {
        qint64 ptsMs = 0;
        QVariantMap payload;
    };

    QList<QScreen*> m_screens;
    QVariantList m_screenOptions;

    ProjectImportClient *m_importClient = nullptr;
    TelemetryClient *m_telemetryClient = nullptr;
    ProjectSettingsImporter m_settingsImporter;
    ProjectSettingsImportResult m_pendingImport;
    bool m_hasPendingImport = false;
    QString m_importPreviewError;
    QVariantMap m_importPreview;
    QVariantMap m_liveTelemetry;
    QMap<QString, QList<TelemetryTimelineEntry>> m_recordingTelemetry;
    TelemetryTimelineReader m_telemetryTimelineReader;
    bool m_hasTelemetryTimeline = false;
    int m_telemetryVersion = 0;
    int m_broadcastOutputsVersion = 0;
    int m_broadcastOutputStatusVersion = 0;
    QTimer m_broadcastOutputStatusTimer;
    quint64 m_broadcastOutputStatusFingerprint = 0;

    enum MidiLearnMode {
        LearnControl = 0,
        LearnJogForward = 1,
        LearnJogBackward = 2
    };
    int m_midiLearnMode = LearnControl;
};

#endif // UIMANAGER_H
