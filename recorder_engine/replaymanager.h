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
#include "recorder_engine/codec/videocodecchoice.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#include "timing/timecodealigner.h"
#include "timing/sourceoffsetestimator.h"

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
    void setVideoCodec(VideoCodecChoice codec) { m_videoCodec = codec; }
    VideoCodecChoice videoCodec() const { return m_videoCodec; }

    // Getters
    QString getOutputDirectory() const { return m_outputDir; }
    QString getBaseFileName() const { return m_baseFileName; }
    int getVideoWidth() const { return m_videoWidth; }
    int getVideoHeight() const { return m_videoHeight; }
    int getFps() const { return m_fps; }

    int64_t getElapsedMs();
    QString getVideoPath();
    qint64 getRecordingStartEpochMs() const { return m_recordingStartEpochMs; }

    // Inter-camera timecode alignment (Phase 4 consumes these). True iff both
    // sources carried a common timecode AND their equal-TC frames coincide
    // exactly. sourceFrameOffset() is the frame correction to ADD to source B's
    // mapping so its equal-TC frames coincide with A (0 if either lacks TC).
    bool sourcesFrameAligned(int a, int b) const { return m_tcAligner.sourcesAligned(a, b, 0); }
    int64_t sourceFrameOffset(int a, int b) const { return m_tcAligner.frameOffset(a, b); }

    // Inter-camera phase servo evidence (Phase 4). The reference source is the
    // session anchor — the connected source with the highest ClockQuality (ties
    // broken by lowest index); -1 until any source has reported stats. The other
    // accessors expose the SourceOffsetEstimator result the servo (Task 4) and the
    // UI (Task 5) consume: each source's confidence tier, its measured phase to the
    // reference in ms, and the +/-ms bound on that phase. The reference's own phase
    // is 0 by construction. These are purely observational — additive, no effect on
    // recording/sync/stats until a later task wires the correction.
    int referenceSource() const { return m_referenceSource; }
    ConfidenceTier sourceTier(int sourceIndex) const { return m_offsetEstimator.tier(sourceIndex); }
    int64_t sourcePhaseOffsetMs(int sourceIndex) const {
        return m_offsetEstimator.offsetMs(sourceIndex);
    }
    int sourcePhaseBoundMs(int sourceIndex) const { return m_offsetEstimator.boundMs(sourceIndex); }

    // Phase-4 servo cap: the maximum magnitude (ms) of the inter-camera phase
    // correction applied per source. DELIBERATELY small (a few frames) — the servo
    // gently nudges a follower toward the reference, never fights the operator trim,
    // and a saturating phase reading clamps here rather than distorting the timeline.
    static constexpr int kMaxInterCamCorrectionMs = StreamWorker::kMaxServoTrimMs;
    // Max servo movement per stats pulse (ms). The ramp: a sudden/stepping Bounded
    // phase reading moves the servo by at most this much per update, so a re-anchor
    // step can never jerk the timeline by the full correction in one tick.
    static constexpr int kServoStepMs = 4;

    // The current inter-camera servo trim (ms) applied to a source's worker — the
    // ramped, capped correction toward the reference. 0 for the reference, for
    // Approximate sources, and for a lone source. Observational accessor for tests/UI.
    int sourceServoTrimMs(int sourceIndex) const {
        return (sourceIndex >= 0 && sourceIndex < m_servoTrimMs.size()) ? m_servoTrimMs[sourceIndex]
                                                                        : 0;
    }

signals:
    // Emitted once per advanced frame: (global frame index, elapsed ms
    // since recording start).  The second value is MILLISECONDS — it was
    // previously named wallClockUs.
    void masterPulse(int64_t frameIndex, int64_t elapsedMs);

    // Relayed from each StreamWorker when its connection state flips.
    // sourceIndex is the fixed source identity (not a view slot).
    void sourceConnectionChanged(int sourceIndex, bool connected);

    // Relayed from each StreamWorker ~1/sec with that source's latest ingest stats.
    void sourceStatsUpdated(int sourceIndex, IngestStats stats);

    // Emitted after a per-feed telemetry packet has been stamped and written.
    void telemetryRecorded(const QString &feedId, const QJsonObject &payload, qint64 effectiveMs);

private slots:
    void onTimerTick();

    // Queued from each StreamWorker::frameTimecode. Records the source's per-frame
    // timecode (100 ns since midnight) against the session frame index it landed on
    // so two sources' equal-TC frames can be compared (sourcesFrameAligned).
    void onFrameTimecode(int sourceIndex, int64_t sourceTimecode100ns, int64_t sessionFrameIndex);

    // Queued from each StreamWorker::statsUpdated (~1/sec). Caches the source's
    // latest IngestStats, re-runs the inter-camera phase estimation, then STAMPS the
    // additive Phase-4 fields (confidenceTier/interCamPhaseMs/interCamBoundMs/
    // isReference) from the estimator onto the relayed copy and emits it as
    // sourceStatsUpdated for the UI — every PRE-EXISTING field stays byte-identical;
    // only the additive estimator fields are augmented (no new signal).
    void onSourceStatsUpdated(int sourceIndex, IngestStats stats);
    // Drops a source from inter-cam phase eligibility on disconnect + re-selects
    // the reference, so the servo never corrects toward a dead reference.
    void onSourcePhaseConnectionChanged(int sourceIndex, bool connected);

private:
    void writeBlueFrames(int64_t elapsedMs);

    // Re-pick the reference source (highest ClockQuality among sources that have
    // reported stats, ties -> lowest index) and re-grade every source's inter-camera
    // phase + confidence tier from the cached IngestStats + the timecode aligner.
    // Cheap and pure of side effects beyond m_referenceSource/m_offsetEstimator;
    // called on every stats pulse (no new timer — stats already arrive ~1/sec).
    void recomputeInterCamPhase();

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
    VideoCodecChoice m_videoCodec = VideoCodecChoice::Mpeg2Software;

    QTimer* m_heartbeat;
    Muxer* m_muxer;
    RecordingClock* m_clock;
    QList<StreamWorker*> m_workers;  // One per SOURCE (not per view)

    // Inter-camera timecode aligner. Constructed with the SHARED
    // Smpte12m::kTimecodeNominalFps (30), NOT m_fps. WHY 30 and not m_fps: the
    // SRT/RTMP producers encode each frame's TC to the carried 100 ns with this
    // same constant, and the aligner decodes that 100 ns back to a TC frame count
    // with it — using one shared constant makes producer and consumer provably
    // agree (the value cancels in the round-trip). Inter-camera alignment is
    // anchor-relative (a difference of two sources' frame mappings), so it is
    // correct at ANY source rate; only the ABSOLUTE TC->frame mapping is exact
    // solely at 30 — a documented limitation. Threading the real per-source fps
    // end-to-end is a future refinement.
    TimecodeAligner m_tcAligner{Smpte12m::kTimecodeNominalFps};

    // Inter-camera phase servo (Phase 4). m_offsetEstimator grades each source's
    // offset-to-reference + confidence tier; m_referenceSource is the chosen anchor
    // (-1 until the first stats pulse); m_lastStats caches the most recent IngestStats
    // per source so recomputeInterCamPhase can difference them. All reset on
    // startRecording (mirrors m_tcAligner.reset()). m_sourceHasStats marks which
    // sources have reported (i.e. are connected) so reference selection considers
    // only live sources.
    SourceOffsetEstimator m_offsetEstimator;
    int m_referenceSource = -1;
    QList<IngestStats> m_lastStats;
    QList<bool> m_sourceHasStats;
    // Current per-source inter-cam servo trim (ms), the ramped+capped correction last
    // pushed to each worker. Persists across pulses so the ramp accumulates gently
    // toward the target; reset to 0 on startRecording. Grows on demand like m_lastStats.
    QList<int> m_servoTrimMs;

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
    // H.264 path: native encoder used for priming the blue frame to obtain avcC.
    std::unique_ptr<NativeVideoEncoder> m_blueNativeEncoder;
    // avcC extradata obtained from priming encode; passed to Muxer::init for H.264.
    // Empty for MPEG-2 (not needed).
    QByteArray m_videoExtradata;
    bool setupBlueEncoder();
    void cleanupBlueEncoder();

    // Per-view silence cursor (sample index @ 48 kHz) so unmapped views
    // get gap-free audio even when heartbeat ticks are missed.
    QVector<int64_t> m_blueAudioCursor;
};

#endif // REPLAYMANAGER_H
