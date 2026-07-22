#ifndef PLAYBACKWORKER_H
#define PLAYBACKWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QVector>
#include <QMutex>
#include <QList>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include "frameprovider.h"
#include "playback/commitgate.h"
#include "playback/frameindex.h"
#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/gpuseekprefetch.h"
#include "playback/gpu/gpuframeretirequeue.h"
#endif
#include "playback/output/colormetadata.h"
#include "playback/output/outputframecache.h"
#include "playback/output/outputruntime.h"
#include "playback/output/sharedcacheslot.h"
#include "playback/output/outputtargetassignment.h"
#include "playback/playbacktransport.h"
#include "playback/audioplayer.h"
#include "playback/trackbuffer.h"
#include "playback/audioframequeue.h"
#include "recorder_engine/ingest/nativevideodecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

ColorMetadata colorMetadataForAvFrame(const AVFrame* frame);

class DecodeDoneFence;
class GpuFence;
class GpuRhiContext;
class QSemaphore;
#if defined(OLR_GPU_PIPELINE_BUILD) && defined(_WIN32)
class WinGpuImportEdge;
#endif

struct DecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    // Hardware H.264 decode: when set, this track is decoded via NativeVideoDecoder
    // instead of the FFmpeg software codecCtx (which stays nullptr for H.264 tracks).
    std::unique_ptr<NativeVideoDecoder> nativeDecoder;
    H26xParameterSets h264ParamSets; // SPS/PPS parsed from avcC extradata at open time
    // Dimensions for the output-graph init when codecCtx is null (H.264 tracks).
    int codecWidth = 0;
    int codecHeight = 0;
    FrameProvider* provider = nullptr;
    int streamIndex = -1;
    int feedIndex = -1;
    TrackBuffer buffer;
    int64_t lastDeliveredPtsMs = -1; // last frame released to the provider
    int decimateCounter = 0;         // per-track keep-counter (§6.3 decimation)
    int nativeDecodeFailureWarnings = 0;
};

struct AudioDecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    int streamIndex = -1;
    int viewIndex = -1;             // which view (0..N-1) this audio belongs to
    int64_t lastEnqueuedPtsMs = -1; // for dedup-before-decode after EOF un-latch
    int64_t lastCachedPtsMs = -1;   // output-bus audio cache dedup
};

class PlaybackWorker : public QThread {
    Q_OBJECT
#ifdef OLR_UNIT_TEST
    friend class TestStagingFence;
    friend class TestPlaybackWorker;
    friend class TestGpuDeviceLostWorker;
    friend class WinGpuFaultWorkerOracle;
    std::atomic<qint64> m_gpuLastAbandonedRetainsForTest{0};
    QSemaphore* m_gpuBeforeTokenlessRecoveryEnteredForTest = nullptr;
    QSemaphore* m_gpuContinueTokenlessRecoveryForTest = nullptr;
    QSemaphore* m_gpuBeforeRecoveryCommitForTest = nullptr;
    QSemaphore* m_gpuContinueRecoveryCommitForTest = nullptr;
#endif
public:
    struct ResidencyWindowParams {
        int leadMs = 500;
        int trailMs = 300;
        int chunkMs = 500;
        int slackMs = 200;
        int audioTrailMs = 500;
        int globalFrameBudget = 256;
        int perTrackCapOverride = 0;
    };

    struct PlaybackCounters {
        int reposition = 0, reuseSeek = 0, reverseChunkSeek = 0, eofTailSeek = 0, skipForward = 0,
            audioPushes = 0, framesDropped = 0;
        // Operator/manual seeks served inline from the already-published output cache
        // (requestSeekTo's committedFromPublishedCache fast path). This is window reuse
        // that never reaches the worker-loop reuseAt path, so reuseSeek does NOT count
        // it: a retained trail window makes paused stepping resolve here (no reposition,
        // no re-decode). The stepscrub gate counts reuseSeek+publishedSeek as reuse.
        int publishedSeek = 0;
        // Repositions issued by the armed-cut decoder-follow (a backward cut's
        // deterministic primary-bank resync). Counted SEPARATELY from reposition
        // so the armed-cut gate keeps reposition==0 (no coarse-seek fallback)
        // while still observing the follow fired exactly once.
        int cutFollowReposition = 0;
        // Video frames committed to the output cache via insertVideoFrame. Counts
        // every decoded frame regardless of whether an output sink is connected,
        // so the e2e gate can prove real decode happened without an NDI/display
        // sink. A stuck or absent decoder leaves this at 0. NOTE: incremented only
        // on the PRIMARY decode bank (decodePacketIntoBank); the pre-roll staging
        // fill is counted separately by stagingVideoFramesDecoded below.
        qint64 decodedVideoFrames = 0;
        // Video frames the armed-cut pre-roll committed into m_prerollStagingCache
        // (fillStaging). This is the DIRECT, unfakeable non-vacuity proof that the
        // staging bank actually decoded the target window before a cut promoted it
        // — distinct from decodedVideoFrames (primary bank). A cut that "fires" off
        // an empty/dry staging cache (riding the dispatcher's hold-last) leaves this
        // at 0, so the H.264 armed-cut gate asserts a floor on it. Counts both the
        // native (H.264) and the FFmpeg (MPEG-2) staging paths.
        qint64 stagingVideoFramesDecoded = 0;
        // GPU-backed frames materialized to CPU planes. Stays 0 on the CPU path;
        // Phase-2 macOS GPU playback increments it only when a sink/preview asks
        // a GpuFrameData to read back.
        qint64 gpuReadToCpuCount = 0;
        qint64 gpuSeekPrefetchConsults = 0;
        qint64 gpuSeekPrefetchPlannedSurfaces = 0;
        qint64 gpuSeekPrefetchGpuAttempts = 0;
        qint64 gpuMemoryPressureLevel1 = 0;
        qint64 gpuMemoryPressureLevel2 = 0;
        qint64 transportPlayheadMs = 0;
        qint64 committedPlayheadMs = 0;
        qint64 lastVisiblePlayheadMs = 0;
        uint64_t seekGeneration = 0;
        uint64_t committedGeneration = 0;
        uint64_t committedGpuGeneration = 0;
        uint64_t currentGpuGeneration = 0;
        bool outputPlayheadCacheGuarded = false;
        int forceLiveOutputSnapshots = 0;
        bool memoryPressureLatched = false;
        int gpuPipelineState = 0;
    };

    explicit PlaybackWorker(const QList<FrameProvider*>& providers, PlaybackTransport* transport,
                            AudioPlayer* audioPlayer = nullptr, QObject* parent = nullptr);
    ~PlaybackWorker();

    struct OperatorSeekResult {
        bool completed = false;
        bool submittedPgm = false;
        bool timedOut = false;
        qint64 targetMs = 0;
        uint64_t generation = 0;
        OutputFrameIdentity pgmIdentity;
        qint64 elapsedNs = 0;
        QString message;
    };

    void openFile(const QString& filePath);
    void seekTo(int64_t timestampMs, int directionHint = 0);
    OperatorSeekResult seekToAndWaitForPgm(qint64 timestampMs, int directionHint, int timeoutMs);
    // Non-blocking transactional seek: registers the operator transaction, performs
    // the inline cache-hit PGM dispatch when the target is already covered, and
    // returns the generation. Completion is reported via operatorSeekCompleted.
    quint64 seekToWithPgmNotify(qint64 timestampMs, int directionHint);
    // Abandon a still-waiting operator transaction (thread-safe). Mirrors the
    // blocking wait loop's timeout behavior: the worker will not later complete
    // it or submit PGM for it. No-op if the generation does not match or the
    // transaction already completed.
    void abandonOperatorSeekTransaction(quint64 generation);
    // Tier3 frame-perfect ARMED CUT: arm a scheduled atomic cut to targetMs.
    // UI-thread-safe (atomic stores only, never blocks). The worker pre-rolls
    // [target, target+kStagingSpanMs] into a private staging cache on a SECOND
    // AVFormatContext while the primary keeps playing, then promotes staging ->
    // active at a scheduled output frame (makeOutputSnapshot) with zero gray and
    // zero reposition. v1 is single-clip (ms-only; same currently-open file). If
    // the pre-roll context failed to open, this is a no-op (feature unavailable).
    // Returns true when the cut was armed (or queued for re-arm), false when the
    // armed-cut feature is unavailable (e.g. H.264: the pre-roll bank is empty).
    // Callers that need navigation even when arming fails should seekPlayback on
    // false — that keeps Recall functional on H.264 recordings.
    //
    // fireAtPlayheadMs: when >= 0, the cut fires when the playhead reaches this
    // position (e.g. a playlist entry's out-point) rather than as-soon-as-staged,
    // for frame-perfect playout transitions. -1 (default, e.g. a Recall) = fire ASAP.
    bool armNextCut(int64_t targetMs, int64_t fireAtPlayheadMs = -1);
    // True if the frame-perfect armed cut is available (the pre-roll context opened).
    // False on recordings without an all-intra pre-roll bank (e.g. H.264), where
    // armNextCut returns false. Callers that depend on armed cuts (playlist playout)
    // gate on this to fail fast rather than silently dead-end. Read from the UI
    // thread, mirroring armNextCut's own m_prerollFmtCtx check.
    bool armedCutAvailable() const { return m_prerollFmtCtx != nullptr; }
    // Direction-aware delivery (spec §5): forward delivers iff pts moved up,
    // reverse iff pts moved down (dir = +1 / -1).
    void deliverDueFrames(int64_t P, int dir);
    void setActiveAudioView(int viewIndex);
    void setSelectedOutputFeed(int feedIndex);
    void setRequireAllOutputFeedsForPlayhead(bool required);
    void setBusPreviewProviders(FrameProvider* multiviewProvider, FrameProvider* pgmProvider);
    void setFeedPreviewProvidersEnabled(bool enabled);
    void setExternalOutputTargets(const QList<OutputTargetAssignment>& assignments);
    void resetOutputPlayEpoch();
#ifdef OLR_UNIT_TEST
    class OutputCommitBarrierForTest {
    public:
        virtual ~OutputCommitBarrierForTest() = default;
        virtual void enterAndWait() = 0;
    };

    void setOutputCommitBarrierForTest(OutputCommitBarrierForTest* barrier);
    void setResidencyWindowParamsForTest(const ResidencyWindowParams& params);
    static int64_t liveGrowthFileSizeForTest(int64_t avioSize, const QString& filePath);
    static int64_t liveEofRecoveryAnchorMsForTest(int64_t playheadMs, int64_t newestBeforeEofMs,
                                                  int64_t trailMs, int64_t frameDurationMs);
    static bool liveReadDeadlineInterruptsForTest(bool baseInterrupt, int64_t deadlineMs,
                                                  int64_t nowMs);
#ifdef OLR_GPU_PIPELINE_BUILD
    void evaluateGpuMemoryPressureForTest(uint64_t availableBytes, bool memoryWarning,
                                          qint64 nowMs = 0);
    static int64_t manualSeekCommitFillToForTest(int64_t target, int64_t frameDurationMs,
                                                 const GpuPrefetchPlan& prefetchPlan);
#endif
#endif
    void stop();

    PlaybackCounters counters() const;
    OutputDispatchStats outputStats() const;
    uint64_t gpuGeneration() const;
#ifdef OLR_GPU_PIPELINE_BUILD
    void injectGpuDeviceLossForTest();
#endif
    // The committed cache generation (set at repositionTo's tail). >=1 after a
    // real reposition proves a target was decoded and committed to the cache.
    uint64_t cacheGeneration() const {
        return m_committedGeneration.load(std::memory_order_acquire);
    }
    // Number of armed cuts that have fired (promoted staging -> live). A queued
    // re-arm (armNextCut while a cut is in flight) that is applied yields a
    // SECOND fired cut, so this proves the safe re-arm queue actually fired the
    // latest target rather than dropping it.
    int cutsFired() const { return m_cutsFired.load(std::memory_order_acquire); }

signals:
    // Emitted (queued consumers only — connect with explicit Qt::QueuedConnection,
    // result passed by value) whenever an operator transaction resolves:
    //   completed && submittedPgm            -> PGM accepted the committed target
    //   !submittedPgm, message == "PGM output was not submitted" -> dispatch failed
    //   !completed, message == "superseded"  -> a newer seek replaced it
    void operatorSeekCompleted(quint64 generation, PlaybackWorker::OperatorSeekResult result);

protected:
    void run() override;

private:
    enum class OutputCoverageMode {
        OperatorSeek,
        StrictSeek,
        Displayable,
    };

    enum class OutputCacheAction : uint8_t { Keep, Publish, MergeStagingAndPublish };
    enum class PostCommitDispatch : uint8_t { None, Output, Preview, PgmCritical };

    struct OutputCommit {
        qint64 playheadMs = 0;
        uint64_t seekGeneration = 0;
        uint64_t gpuGeneration = 0;
        OutputCacheAction cacheAction = OutputCacheAction::Keep;
        OutputCoverageMode coverageMode = OutputCoverageMode::StrictSeek;
        bool requireCurrentSeek = true;
        bool clearSeekTarget = false;
        bool guardPlayheadCache = false;
        // When false, the commit skips the coverage/displayable gate and commits at
        // playheadMs unconditionally (used by a scheduled cut that has deferred to its
        // bound: the cut must land on air even if a feed is not yet displayable).
        bool requireCoverage = true;
        PostCommitDispatch dispatch = PostCommitDispatch::None;
    };

    struct OutputCommitResult {
        bool committed = false;
        qint64 committedPlayheadMs = 0;
        uint64_t committedGeneration = 0;
        PostCommitDispatch dispatch = PostCommitDispatch::None;
    };

    struct SeekRequestResult {
        qint64 clampedTargetMs = 0;
        int moveDir = 1;
        uint64_t generation = 0;
        bool committedFromPublishedCache = false;
        PostCommitDispatch dispatch = PostCommitDispatch::None;
        qint64 publishNs = 0;
    };

    struct OperatorSeekCompletionState {
        uint64_t generation = 0;
        qint64 targetMs = -1;
        bool waiting = false;
        bool completed = false;
        bool pgmDispatchAttempted = false;
        bool submittedPgm = false;
        OutputFrameIdentity pgmIdentity;
        QString message;
    };

    // --- Scheduler constants (spec §3) ------------------------------------
    static constexpr int kLeadMs = 500;            // default video window ahead of P
    static constexpr int kTrailMs = 300;           // default video window behind P
    static constexpr int kChunkMs = 500;           // default reverse backward-fetch chunk size
    static constexpr int kAudioLeadMs = 200;       // max lead of pushed audio over P
    static constexpr int kAudioQueueMs = 900;      // worker audio-queue span bound
    static constexpr int kSlackMs = 200;           // trim hysteresis beyond the window
    static constexpr int kIdleSleepMs = 3;         // sleep when window full and playing
    static constexpr int kEofSleepMs = 10;         // sleep between EOF re-checks
    static constexpr int kReadErrSleepMs = 20;     // sleep after a non-EOF read error
    static constexpr int kLiveReadTimeoutMs = 100; // bound av_read_frame on growing local files
    static constexpr int kBackJumpSlackMs = 150;   // P below buffered span by this ⇒ reposition
    static constexpr int kGlobalFrameBudget = 256; // aggregate decoded-frame cap (memory)
    static constexpr double kDecimateAbove = 1.5;  // |speed| above which decimation engages

    // --- Tier3 pre-roll / armed-cut constants -----------------------------
    static constexpr int kStagingSpanMs = 800;       // window staged ahead of target
    static constexpr int kPrerollPacketsPerTick = 8; // bounded per run() iter (no starve)
    static constexpr int kCutLeadMs = 120;           // lead before the cut fires (output frames)
    static constexpr int kPrerollAudioSpanMs = 800;  // active-view audio staged ahead of a cut

    // High-performance conversion from FFmpeg AVFrame to backend YUV420P media frames.
    FrameHandle convertToMediaVideoFrame(AVFrame* frame, int feedIndex);

    // --- Scheduler helpers (spec §3 symbols / §6). Task 5 wires the loop;
    //     bodies are implemented here except repositionTo (stubbed). ---------
    int fps() const;            // m_transport->fps(), clamped >=1
    int64_t frameDurMs() const; // 1000 / fps()
    int64_t maxPriorCoverageMs() const;
    std::optional<qint64>
    outputFeedCoverageInCache(const OutputFrameCache& cache, int feedIndex, int64_t playheadMs,
                              uint64_t gpuGeneration,
                              OutputCoverageMode mode = OutputCoverageMode::StrictSeek) const;
    bool
    outputFeedCoversPlayheadLocked(int feedIndex, int64_t playheadMs, uint64_t gpuGeneration,
                                   OutputCoverageMode mode = OutputCoverageMode::StrictSeek) const;
    bool outputCacheCoversPlayheadInCacheLocked(const OutputFrameCache& cache, int64_t playheadMs,
                                                uint64_t gpuGeneration,
                                                OutputCoverageMode mode) const;
    bool outputCacheCoversPlayheadLocked(
        int64_t playheadMs, uint64_t gpuGeneration,
        OutputCoverageMode mode = OutputCoverageMode::OperatorSeek) const;
    std::optional<qint64> outputCacheDisplayablePlayheadInCacheLocked(const OutputFrameCache& cache,
                                                                      qint64 playheadMs,
                                                                      uint64_t gpuGeneration) const;
    std::optional<qint64> outputCacheDisplayablePlayheadLocked(qint64 playheadMs,
                                                               uint64_t gpuGeneration) const;
    std::optional<qint64>
    validatedOutputCommitPlayheadLocked(const OutputCommit& commit,
                                        const OutputFrameCache* coverageCache) const;
    OutputCommitResult commitOutputStateLocked(const OutputCommit& commit);
    // Whether a still-in-flight operator PGM obligation should be (re)dispatched at a
    // reposition commit. The per-packet early-completion (tryComplete) is one-shot via
    // OperatorSeekCompletionState::pgmDispatchAttempted, but the final reposition commit
    // makes the last PGM attempt for the SAME seek, so a transient early miss does not
    // strand the operator's take-to-air. Caller holds m_mutex.
    bool operatorPgmObligationAvailableLocked(uint64_t generation) const;
    OutputCommitResult commitFullRepositionOutputStateLocked(
        const OutputCommit& commit, std::unique_ptr<OutputFrameCache>& liveSaved, qint64 keepFrom,
        qint64 keepTo, qint64 keepAudioFromSample, bool sanitizeForDeviceLoss);
    bool outputCacheCoversPlayhead(int64_t playheadMs) const;
    bool pausedPlayheadNeedsWork(int64_t playheadMs);
    SeekRequestResult requestSeekTo(qint64 timestampMs, int directionHint,
                                    bool registerOperatorTransaction);
    OutputDispatchReport dispatchPgmAfterSeekCommit(qint64 targetMs);
    OutputDispatchReport dispatchPgmCommitObligation(qint64 targetMs, uint64_t generation);
    void completeOperatorSeekTransaction(uint64_t generation, qint64 targetMs,
                                         const OutputDispatchReport& report);
    bool hasOperatorSeekTransaction(uint64_t generation);
    bool tryCompleteOperatorSeekFromCurrentOutputCache(qint64 targetMs, uint64_t generation);
    void maybeCompleteOperatorSeekAfterDecodedPacket(qint64 targetMs, uint64_t generation,
                                                     bool& operatorPgmCompletedEarly);
    bool allowDisplayableFallbackForReposition(uint64_t generation);
    int64_t windowLeadMs() const;
    int64_t windowTrailMs() const;
    int64_t windowChunkMs() const;
    int64_t windowSlackMs() const;
    int64_t windowAudioTrailMs() const;
    int64_t liveGrowthFileSize() const;
    int capFrames(int trackCount) const;
    int64_t newestPtsMin() const; // min-newest, staleness-excluded; -1 empty
    int64_t oldestPtsMin() const; // min-oldest, staleness-excluded; -1 empty
    int64_t newestPtsMax() const; // cross-track max-newest (ignoring empty); -1 empty
    int64_t refNewestPts() const; // reference (first) track newest; -1 empty
    int64_t refOldestPts() const; // reference (first) track oldest; -1 empty
    // cutFollow=true marks a reposition issued by the armed-cut decoder-follow:
    // it counts cutFollowReposition instead of reposition (the output cache is
    // already correct; this only resyncs the primary decode engine).
    void repositionTo(int64_t target, int dir, AVPacket* pkt, AVFrame* vf, AVFrame* af,
                      bool cutFollow = false);
    // True when decoder buffers and the output cache both cover target for display.
    bool reuseAt(int64_t target);

    // Decode one read packet into the bank (video → insert with cap; audio →
    // enqueue active view). Used by forward fill, reposition, and reverse fill.
    // `decimate` engages count-based decimation; `audioOn` gates audio enqueue.
    // `dedupTail` skips video/audio packets at/behind the owning track's newest
    // (post-EOF-unlatch re-read guard, §6.8). Returns the just-decoded video
    // PTS (ms) of the last frame produced, or INT64_MIN if none.
    int64_t decodePacketIntoBank(AVPacket* pkt, AVFrame* vf, AVFrame* af, int64_t P, int dir,
                                 int trackCount, bool decimate, int decimateStep, bool audioOn,
                                 bool dedupTail);
    void indexPrimaryVideoPacketForSeek(const DecoderTrack* track, const AVPacket* pkt,
                                        qint64 framePtsMs);
    // Enqueue a decoded active-view audio frame onto m_audioQueue (format-guarded).
    void enqueueAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame, bool dedupTail);
    void cacheOutputAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame, bool dedupTail);
    void resetDedup(); // lastDeliveredPtsMs = -1 on every track
    void clearDecoderBuffers(bool invalidateGpuGeneration = true);
    // Seek the primary demuxer/decoder bank near target without clearing the
    // published output cache. Used for forward playback catch-up and forward
    // armed-cut primary-bank resyncs; counts as skipForward, not reposition.
    bool resyncPrimaryDecodeCursorTo(qint64 targetMs);
    // clear every TrackBuffer (holds m_bufferMutex); leaves m_outputCache intact
    void initializeOutputGraph(int feedCount, int width, int height);
    void shutdownOutputGraph();
    void rebuildOutputEndpoints();
    OutputRuntimeSnapshot makeOutputSnapshot() const;
    void refreshOutputAfterSeekCommit();
    void refreshPreviewAfterSeekCommit();
    // Snapshot m_outputCache into the published immutable slot. Caller must hold
    // m_bufferMutex.
    void publishOutputCacheLocked();
#ifdef OLR_GPU_PIPELINE_BUILD
    enum class GpuPipelineState { Gpu, RebuildPending, CpuFallback };
    static constexpr int kDeviceLossRebuildBudget = 3;

    void collectEvictedGpuFramesLocked(const TrackBuffer::EvictedFrames& evictedFrames);
    void collectEvictedGpuFramesLocked(const OutputFrameCache::EvictedVideoFrames& evictedFrames);
    void collectEvictedGpuFrameLocked(const FrameHandle& frame);
    void drainEvictedGpuFrames();
    void forceDrainEvictedGpuFrames();
    void recordFenceWaitStall();
    bool ensureWindowsGpuImportFencesReadyForDecode();
    void configureGpuBudget();
    GpuPipelineState gpuPipelineState() const;
    bool gpuPathActive() const;
    bool gpuLifecycleSuspended() const;
    bool gpuDeviceLossPending() const;
    bool consumeGpuDeviceLossRebuildBudget();
    void drainGpuDeviceLossEvents() const;
    void cleanupGpuRetirementsForDeviceLoss(bool allowTokenlessTestGate, bool pollBackends = true);
    bool completeCoordinatedGpuRebuild(bool consumeRebuildBudget);
    void handleGpuDeviceLoss();
    void sampleGpuMemoryPressure(qint64 nowMs);
    void evaluateGpuMemoryPressure(uint64_t availableBytes, bool memoryWarning, qint64 nowMs);
    qint64 gpuMemoryPressureLevel1ThresholdBytes();
    bool deriveGpuBudgetFromAvailableMemory(uint64_t availableBytes);
    void handleGpuMemoryPressureLevel1(qint64 nowMs);
    void handleGpuMemoryPressureLevel2(qint64 nowMs);
    void flushNativeDecoderPools();
    void flushNativeDecoderPoolsThrottled(qint64 nowMs);
    void resumeDeferredGpuRebuild();
    void detachOutputEndpointsForDeviceLoss();
    void sanitizeCacheForDeviceLossLocked(OutputFrameCache* cache, int* recoveredFrames = nullptr,
                                          int* removedGpuFrames = nullptr);
    void sanitizeTrackBufferForDeviceLossLocked(TrackBuffer* buffer, int* recoveredFrames = nullptr,
                                                int* removedGpuFrames = nullptr);
    std::optional<qint64> recoveredCachePlayheadLocked(qint64 playheadMs,
                                                       uint64_t gpuGeneration) const;
    bool rebuildGpuSpine();
    static int64_t manualSeekCommitFillTo(int64_t target, int64_t frameDurationMs,
                                          const GpuPrefetchPlan& prefetchPlan);
    GpuPrefetchPlan planGpuSeekPrefetchForReposition(int64_t target, int dir);
    GpuPrefetchPlan beginGpuSeekPrefetchForReposition(int64_t target, int dir);
    void endGpuSeekPrefetchForReposition();
    bool allowNativeGpuDecodeForCurrentPacket(int64_t packetPtsMs);
#endif

    // --- Tier3 pre-roll / armed-cut (worker-thread internals) -------------
    // Open a SECOND independent AVFormatContext on the same clip + its own
    // decoder bank, writing the preroll members. Returns false on failure
    // (pre-roll silently disabled; armNextCut becomes a no-op). Called once in
    // run() after the primary bank + output graph are up.
    bool openPrerollContext();
    // Bounded incremental pre-roll into m_prerollStagingCache (worker-private;
    // NOT published until the cut swap, so no lock during fill). On first call
    // after arm it av_seek_frame's the preroll context BACKWARD to the trail
    // anchor, then decodes forward until the staging cache covers
    // [target, target+kStagingSpanMs]; schedules the cut once covered.
    void fillStaging();
    // Arm the cut state (target + staging reset). Caller MUST guarantee no cut is
    // in flight (m_cutArmed false). Called from armNextCut (UI thread, fresh arm)
    // and from the run loop (worker thread, applying a queued re-arm). Atomics
    // only — no cache touch — so it is safe from either thread. baselineSeekGen is
    // the m_seekGeneration captured when the recall was ISSUED (arm time for a
    // fresh arm, QUEUE time for a re-arm) — stored as m_armSeekGen so any manual
    // seek after that point aborts the cut at fire time (manual-seek-wins policy).
    void armCutInternal(int64_t targetMs, uint64_t baselineSeekGen, int64_t fireAtPlayheadMs);
    // Store the atomic schedule (output frame index + target ms).
    void scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs);
    void markStagingCovered();
    bool stagingGpuSurfacesIdle() const;
    // Fire the scheduled cut iff the dispatcher's next index reached it: swaps
    // staging -> active, republishes, re-bases the transport playhead. MUST be
    // called holding m_mutex -> m_bufferMutex (invoked from makeOutputSnapshot).
    PostCommitDispatch maybeFireScheduledCut(qint64 dispatcherNextIndex);

    static int ffmpegInterruptCallback(void* opaque);
    bool shouldInterrupt() const;
    bool shouldInterruptFfmpeg() const;
    bool liveReadDeadlineExpired(int64_t nowMs) const;
    int readPrimaryFrame(AVPacket* pkt);

    QList<FrameProvider*> m_providers;
    FrameProvider* m_multiviewPreviewProvider = nullptr;
    FrameProvider* m_pgmPreviewProvider = nullptr;
    QVector<DecoderTrack*> m_decoderBank;
    QVector<AudioDecoderTrack*> m_audioDecoderBank;
    AVFormatContext* m_fmtCtx = nullptr;

    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_liveReadDeadlineSteadyMs{-1};
    int64_t m_seekTargetMs = -1;
    OperatorSeekCompletionState m_operatorSeekCompletion;
    QString m_currentFilePath;
    PlaybackTransport* m_transport;
    AudioPlayer* m_audioPlayer = nullptr;
    std::atomic<int> m_activeAudioView{-1};
    std::atomic<int> m_selectedOutputFeed{-1};
    std::atomic<bool> m_requireAllOutputFeedsForPlayhead{false};
    std::atomic<bool> m_feedPreviewProvidersEnabled{false};

    AudioFrameQueue m_audioQueue; // worker-thread-only
    ResidencyWindowParams m_residencyWindowParams;
    qint64 m_decodedVideoSequence = 0;       // worker-thread-only decoded frame identity
    std::atomic<bool> m_audioReprime{false}; // set by setActiveAudioView (UI thread)
    std::atomic<int> m_lastMoveDir{1};
    int64_t m_sizeAtLastEof = -1;
    // Lowest reverse-fetch anchor (ms) issued in the current reverse run.
    // Reverse chunks only re-fetch once the anchor has descended a full
    // kChunkMs, so consecutive chunks tile contiguously instead of
    // re-decoding an overlapping window every iteration. INT64_MAX = no
    // fetch yet this run; reset on reposition and whenever travelling forward.
    int64_t m_reverseAnchorMs = INT64_MAX;

    // PTS(ms) -> byte-offset index of the primary video stream, appended as
    // packets are read (worker-thread-only; no mutex). Kept for diagnostics and
    // future format-specific seek acceleration. Matroska reposition must still
    // enter through avformat/av_seek_frame: raw byte landing can poison demuxer
    // state even for all-intra recordings.
    FrameIndex m_frameIndex;

    // Seek-gate generations (read in makeOutputSnapshot; written in seekTo /
    // repositionTo). When m_committedGeneration == m_seekGeneration there is no
    // reposition outstanding and the live playhead is exposed (1x advances);
    // while they differ a seek is in flight against a not-yet-ready cache and
    // the gate holds m_committedPlayheadMs (CommitGate).
    std::atomic<uint64_t> m_seekGeneration{0};
    std::atomic<uint64_t> m_committedGeneration{0};
    std::atomic<int64_t> m_committedPlayheadMs{0};
#ifdef OLR_GPU_PIPELINE_BUILD
    // GPU generation visible to the output graph once the cache generation is
    // committed. While CommitGate is holding the old cache/playhead during a
    // reposition, output must keep accepting the old generation too.
    std::atomic<uint64_t> m_committedGpuGeneration{1};
    std::atomic<int> m_gpuPipelineState{static_cast<int>(GpuPipelineState::CpuFallback)};
#endif
    // Last playhead actually exposed to the output clock while no seek was
    // pending. A later seek holds this recent, cache-covered position instead
    // of a stale reposition target that steady playback may have trimmed away.
    mutable std::atomic<int64_t> m_lastVisiblePlayheadMs{0};
    mutable std::atomic<bool> m_outputPlayheadCacheGuarded{false};

    QMutex m_mutex;
    QWaitCondition m_workerWake;
    QWaitCondition m_operatorSeekCondition;
    mutable QMutex m_bufferMutex;
    mutable QMutex m_outputRuntimeMutex;

    QList<OutputTargetAssignment> m_externalOutputAssignments;
    std::atomic<bool> m_outputTargetsDirty{false};
    std::unique_ptr<OutputFrameCache> m_outputCache;
    // Worker-thread-only staging buffer: a reposition decodes the target window
    // here, then merges into the live cache and trims old frames only after
    // coverage (double-buffer; never published to the output thread).
    std::unique_ptr<OutputFrameCache> m_stagingCache;

    // --- Tier3 pre-roll / armed-cut state (worker-thread-only unless noted) ---
    // A SECOND independent AVFormatContext + decoder bank on the SAME clip; the
    // primary keeps playing while this pre-rolls the armed target window into a
    // private staging cache. Opened once in run() after the primary bank is up;
    // freed in run() cleanup. If the open fails, the pre-roll is disabled and
    // armNextCut becomes a no-op (the feature is silently unavailable).
    AVFormatContext* m_prerollFmtCtx = nullptr;     // mirrors m_fmtCtx
    QVector<DecoderTrack*> m_prerollBank;           // mirrors m_decoderBank
    QVector<AudioDecoderTrack*> m_prerollAudioBank; // mirrors m_audioDecoderBank
    // Pre-roll target window, sized identically to m_outputCache. Worker-private
    // during the fill (never published) — swapped into m_outputCache at the cut.
    std::unique_ptr<OutputFrameCache> m_prerollStagingCache;
    // Armed-cut control. Set by armNextCut (UI thread, atomic stores only).
    std::atomic<int64_t> m_armedTargetMs{-1};
    std::atomic<bool> m_cutArmed{false};
    std::atomic<bool> m_prerollSeekPending{false};
    // True once the staging cache covers [target, target+span]. Atomic because
    // armNextCut clears it from the UI thread while the worker reads/writes it.
    std::atomic<bool> m_stagingCovers{false};
#ifdef OLR_GPU_PIPELINE_BUILD
    std::shared_ptr<GpuFence> m_stagingFence;
    std::atomic<uint64_t> m_stagedFenceValue{0};
#endif
    // Newest video PTS (ms) currently staged for the reference feed (feed 0),
    // tracked during fillStaging since OutputFrameCache has no newest accessor.
    int64_t m_stagingNewestRefPtsMs = INT64_MIN;
    // Scheduled cut: the output frame index to fire at + the target ms. Read in
    // maybeFireScheduledCut (under m_bufferMutex); written by scheduleCutAtFrame.
    std::atomic<qint64> m_scheduledCutFrame{-1};
    std::atomic<int64_t> m_scheduledCutTargetMs{-1};
    // A scheduled program cut must land on air. When the promoted staging cache is not
    // yet displayable at the post-cut playhead the fire defers (retries next tick), but
    // only up to kMaxScheduledCutDeferredTicks; after that it fires unconditionally (a
    // brief placeholder or hold-last frame on a lagging feed is acceptable, an
    // indefinitely-deferred cut is not). The bound is kept small because
    // CutSchedule::playheadAfterCut advances the post-cut playhead each deferred tick
    // while staging stays pinned to the armed target, so waiting longer yields a staler
    // forced frame, not a fresher one. Counted and reset on the output thread under
    // m_bufferMutex.
    static constexpr int kMaxScheduledCutDeferredTicks = 3;
    int m_scheduledCutDeferredTicks = 0;
    // Safe re-arm queue: a Recall (armNextCut, UI thread) that arrives while a cut
    // is already in flight stores the LATEST target here instead of dropping it or
    // (unsafely) resetting the staging state mid-cut. The run loop applies it via
    // armCutInternal once the in-flight cut clears m_cutArmed — so the re-arm and
    // its subsequent staging fill happen on the worker thread, never concurrently
    // with the output thread's swap in maybeFireScheduledCut. Latest target wins.
    std::atomic<int64_t> m_pendingRearmMs{-1};
    std::atomic<bool> m_hasPendingRearm{false};
    // m_seekGeneration captured when a re-arm was QUEUED (armNextCut, UI thread).
    // The worker uses it as the cut's seek baseline when it applies the queued
    // re-arm, and drops the re-arm if m_seekGeneration advanced since queuing — so
    // a manual seek issued after the recall (but while the prior cut was in flight)
    // still wins. Without this, capturing the baseline at apply time would re-
    // baseline against the post-seek generation and the recalled cut would fire.
    std::atomic<uint64_t> m_pendingRearmSeekGen{0};
    // Count of fired cuts (incremented in maybeFireScheduledCut, output thread).
    std::atomic<int> m_cutsFired{0};
    // Armed-cut decoder-follow. A BACKWARD cut swaps only the OUTPUT cache + re-bases
    // the playhead, leaving the PRIMARY demuxer+decoder bank parked AHEAD of the new
    // playhead. maybeFireScheduledCut (output thread) stores the new playhead here —
    // BEFORE re-basing the transport playhead, so the worker, on observing the
    // re-based playhead, is guaranteed to also see this and the reactive backward-jump
    // path never fires. The run loop consumes it (exchange) on the worker thread and
    // resyncs the primary bank with a non-clearing repositionTo. -1 = none. Forward
    // cuts leave it unset (the forward-lag skip-forward path resyncs without a
    // reposition). Set on the output thread, consumed on the worker thread; atomic.
    std::atomic<int64_t> m_decoderFollowMs{-1};
    // Armed-cut forward primary-bank resync. A forward cut promotes the target
    // output cache and re-bases transport, but the primary demuxer can still be
    // parked seconds behind. If ordinary forward-lag sees P past the bank's newest
    // it looks like tail-hold, so the worker consumes this one-shot and performs
    // an explicit non-clearing skip-forward once it observes the re-based playhead.
    std::atomic<int64_t> m_forwardCutResyncMs{-1};
    // m_seekGeneration captured when a cut is armed (armCutInternal). A manual
    // seekTo bumps m_seekGeneration; if it differs at fire time the operator
    // issued an explicit seek after arming, so maybeFireScheduledCut ABORTS the
    // cut (the manual seek wins — it services the jump via repositionTo). This is
    // the authoritative manual-seek-vs-in-flight-cut policy: the cut never fires
    // against a playhead the operator has since seeked away from.
    std::atomic<uint64_t> m_armSeekGen{0};
    // Playhead (ms) at which the armed cut should fire, for frame-perfect playlist
    // playout: the cut fires when the playhead reaches this position (a playlist
    // entry's out-point) instead of as-soon-as-staged. -1 = fire ASAP (a Recall).
    // m_armedFireAtMs is the live armed value; m_pendingRearmFireAtMs carries it
    // through the safe re-arm queue (alongside m_pendingRearmMs).
    std::atomic<int64_t> m_armedFireAtMs{-1};
    std::atomic<int64_t> m_pendingRearmFireAtMs{-1};
    // Immutable snapshot of m_outputCache published to the output thread
    // (replaces the per-tick deep copy in makeOutputSnapshot).
    SharedCacheSlot m_publishedCache;
    std::unique_ptr<OutputRuntime> m_outputRuntime;
#ifdef OLR_UNIT_TEST
    OutputCommitBarrierForTest* m_outputCommitBarrierForTest = nullptr;
#endif
    int m_outputRuntimeImmediateDispatches = 0;
    QWaitCondition m_outputRuntimeImmediateDispatchesIdle;
    std::vector<std::unique_ptr<IOutputSink>> m_outputSinks;
#ifdef OLR_GPU_PIPELINE_BUILD
    GpuFrameRetireQueue m_gpuFrameRetireQueue; // guarded by m_bufferMutex
#endif
    std::shared_ptr<GpuRhiContext> m_gpuRhi;
    std::shared_ptr<DecodeDoneFence> m_decodeFence;
#ifdef OLR_GPU_PIPELINE_BUILD
    std::shared_ptr<GpuFence> m_renderFence;
    mutable std::atomic<qint64> m_gpuDeviceLossEvents{0};
    mutable std::atomic<uint64_t> m_gpuLastObservedLossCount{0};
    uint64_t m_gpuRecoveryParticipantId = 0;     // output-graph lifecycle; worker thread mutates
    uint64_t m_gpuLastHandledLossGeneration = 0; // worker thread only
    uint64_t m_gpuPendingRecoveryGeneration = 0; // worker thread only
    std::atomic<bool> m_injectGpuDeviceLossForTest{false};
    std::atomic<bool> m_forceLiveOutputSnapshotsOnNextAttach{false};
    mutable std::atomic<int> m_forceLiveOutputSnapshots{0};
    std::atomic<int> m_gpuDeviceLossRebuildsRemaining{kDeviceLossRebuildBudget};
    std::atomic<bool> m_gpuRebuildDeferredForSuspend{false};
    std::atomic<bool> m_memoryPressureLatched{false};
    uint64_t m_lastIosMemoryWarningCount = 0;   // worker-thread-only
    qint64 m_lastPressureSampleMs = 0;          // worker-thread-only
    qint64 m_lastPressureWarningMs = -1;        // worker-thread-only
    qint64 m_lastPressureLevel1Ms = -1;         // worker-thread-only
    qint64 m_lastNativeDecoderPoolFlushMs = -1; // worker-thread-only
    bool m_gpuSeekPrefetchActive = false;       // worker-thread-only reposition scope
    int m_gpuSeekPrefetchRemaining = 0;
    GpuPrefetchPlan m_gpuSeekPrefetchPlan;
#endif
#if defined(OLR_GPU_PIPELINE_BUILD) && defined(_WIN32)
    std::unique_ptr<WinGpuImportEdge> m_winGpuImportEdge;
    bool m_winGpuImportTried = false;
#endif
    int m_outputFeedCount = 0;
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;

    PlaybackCounters m_counters;
    void emitTelemetry(int64_t P, int64_t newest, double speed);
};

// Registered so operatorSeekCompleted can be delivered across threads via a
// queued connection and extracted from QSignalSpy in tests.
Q_DECLARE_METATYPE(PlaybackWorker::OperatorSeekResult)

#endif // PLAYBACKWORKER_H
