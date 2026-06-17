#ifndef REPLAYMANAGER_H
#define REPLAYMANAGER_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include "recordingclock.h"
#include "muxer.h"
#include "streamworker.h"
#include "ingest/ingestsession.h"

class ReplayManager : public QObject
{
    Q_OBJECT

public:
    explicit ReplayManager(QObject *parent = nullptr);
    ~ReplayManager();

    // Engine Controls used by UIManager
    void startRecording();
    void stopRecording();
    bool isRecording() const {
        QMutexLocker locker(&m_stateMutex);
        return m_isRecording;
    }

    // Source configuration (N sources)
    void setSourceUrls(const QStringList &urls) { m_sourceUrls = urls; }
    void setSourceNames(const QStringList &names) { m_sourceNames = names; }
    void setSourceMetadata(const QList<QByteArray> &metadata) { m_sourceMetadata = metadata; }
    void setSourceTrims(const QList<int>& trims) { m_sourceTrims = trims; }
    QStringList getSourceUrls() const { return m_sourceUrls; }
    QStringList getSourceNames() const { return m_sourceNames; }

    // Per-feed telemetry tracks recorded alongside the replay.
    void setTelemetryFeeds(const QStringList &feedIds, const QStringList &feedNames,
                           const QList<int> &telemetryDelaysMs);
    bool recordTelemetryEvent(const QString &feedId, const QJsonObject &payload);

    // View configuration (M views/tracks)
    void setViewCount(int count) { m_viewCount = count; }
    int viewCount() const { return m_viewCount; }

    // View mapping: viewSlotMap[v] = sourceIndex (-1 = unmapped/blue)
    // This is the ONLY thing that changes during toggle — zero FFmpeg impact.
    void updateViewMapping(const QList<int>& viewSlotMap);

    // For user editing a source URL during recording (real FFmpeg reconnect)
    void updateSourceUrl(int sourceIndex, const QString &url);
    void updateSourceTrim(int sourceIndex, int ms);

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
    // Emitted once per advanced frame: (global frame index, elapsed ms
    // since recording start).  The second value is MILLISECONDS — it was
    // previously named wallClockUs.
    void masterPulse(int64_t frameIndex, int64_t elapsedMs);

    // Relayed from each StreamWorker when its connection state flips.
    // sourceIndex is the fixed source identity (not a view slot).
    void sourceConnectionChanged(int sourceIndex, bool connected);

    // Relayed from each StreamWorker ~1/sec with that source's latest SRT stats.
    void sourceStatsUpdated(int sourceIndex, IngestStats stats);

    // Emitted after a per-feed telemetry packet has been stamped and written.
    void telemetryRecorded(const QString &feedId, const QJsonObject &payload, qint64 effectiveMs);

private slots:
    void onTimerTick();

private:
    void writeBlueFrames(int64_t elapsedMs);

    mutable QMutex m_stateMutex;
    bool m_isRecording = false;
    int64_t m_globalFrameCount = 0;

    // Source config
    QStringList m_sourceUrls;
    QStringList m_sourceNames;
    QList<QByteArray> m_sourceMetadata;  // One JSON blob per source
    QList<int> m_sourceTrims;            // per-source initial trim ms (parallel to m_sourceUrls)

    // Feed telemetry config
    QStringList m_telemetryFeedIds;
    QStringList m_telemetryFeedNames;
    QList<int> m_telemetryDelaysMs;
    QHash<QString, int> m_telemetryFeedIndexById;

    // View config
    int m_viewCount = 4;
    QStringList m_viewNames;
    QList<int> m_viewSlotMap;       // viewSlotMap[v] = source index or -1

    QString m_outputDir;
    QString m_baseFileName;
    QString m_sessionFileName;

    int m_videoWidth = 1920;
    int m_videoHeight = 1080;
    // Heartbeat scheduler cadence — fixed and fps-INDEPENDENT. The recording
    // timeline is wall-clock-derived in onTimerTick (see heartbeatFrameSpan), so the
    // timer is only a scheduler. ~125 Hz oversamples every supported frame rate
    // (<=60 fps); surplus wakes are cheap no-ops (the empty-span early-out).
    // kMaxFramesPerTick caps a post-stall catch-up burst so the GUI thread never
    // freezes; the remainder drains on later ticks.
    static constexpr int kHeartbeatIntervalMs = 8;
    static constexpr int kMaxFramesPerTick = 8;
    int m_fps = 30;

    QTimer* m_heartbeat;
    Muxer* m_muxer;
    RecordingClock* m_clock;
    QList<StreamWorker*> m_workers;  // One per SOURCE (not per view)
    qint64 m_recordingStartEpochMs = 0;

    // Final elapsed captured at stopRecording (before m_clock is deleted) so
    // getElapsedMs() never returns -1 after stop — keeps post-stop snapshot
    // timecodes and QML duration bindings sane.
    int64_t m_lastKnownDurationMs = 0;

    // Blue frame encoder for unmapped views
    AVCodecContext* m_blueEncCtx = nullptr;
    AVFrame* m_blueFrame = nullptr;
    // The blue frame is a static solid color, so its compressed video packet
    // never changes.  We encode it ONCE per recording session and cache the
    // resulting (intra, self-contained) packet here; writeBlueFrames then
    // just clones + re-stamps it per view per pulse — no per-pulse encode on
    // the GUI thread.  Owned by this session: built in setupBlueEncoder,
    // freed in cleanupBlueEncoder.
    AVPacket* m_cachedBluePkt = nullptr;
    bool setupBlueEncoder();
    void cleanupBlueEncoder();

    // Per-view silence cursor (sample index @ 48 kHz) so unmapped views
    // get gap-free audio even when heartbeat ticks are missed.
    QVector<int64_t> m_blueAudioCursor;
};

#endif // REPLAYMANAGER_H
