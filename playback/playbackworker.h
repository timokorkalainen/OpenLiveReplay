#ifndef PLAYBACKWORKER_H
#define PLAYBACKWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QVector>
#include <QMutex>
#include <QList>
#include <atomic>
#include <memory>
#include <vector>
#include "frameprovider.h"
#include "playback/commitgate.h"
#include "playback/frameindex.h"
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
public:
    struct PlaybackCounters {
        int reposition = 0, reuseSeek = 0, reverseChunkSeek = 0, eofTailSeek = 0, skipForward = 0,
            audioPushes = 0, framesDropped = 0;
        // Repositions issued by the armed-cut decoder-follow (a backward cut's
        // deterministic primary-bank resync). Counted SEPARATELY from reposition
        // so the armed-cut gate keeps reposition==0 (no coarse-seek fallback)
        // while still observing the follow fired exactly once.
        int cutFollowReposition = 0;
    };

    explicit PlaybackWorker(const QList<FrameProvider*>& providers, PlaybackTransport* transport,
                            AudioPlayer* audioPlayer = nullptr, QObject* parent = nullptr);
    ~PlaybackWorker();

    void openFile(const QString& filePath);
    void seekTo(int64_t timestampMs);
    // Tier3 frame-perfect ARMED CUT: arm a scheduled atomic cut to targetMs.
    // UI-thread-safe (atomic stores only, never blocks). The worker pre-rolls
    // [target, target+kStagingSpanMs] into a private staging cache on a SECOND
    // AVFormatContext while the primary keeps playing, then promotes staging ->
    // active at a scheduled output frame (makeOutputSnapshot) with zero gray and
    // zero reposition. v1 is single-clip (ms-only; same currently-open file). If
    // the pre-roll context failed to open, this is a no-op (feature unavailable).
    void armNextCut(int64_t targetMs);
    // Direction-aware delivery (spec §5): forward delivers iff pts moved up,
    // reverse iff pts moved down (dir = +1 / -1).
    void deliverDueFrames(int64_t P, int dir);
    void setActiveAudioView(int viewIndex);
    void setSelectedOutputFeed(int feedIndex);
    void setBusPreviewProviders(FrameProvider* multiviewProvider, FrameProvider* pgmProvider);
    void setExternalOutputTargets(const QList<OutputTargetAssignment>& assignments);
    void stop();

    PlaybackCounters counters() const { return m_counters; }
    OutputDispatchStats outputStats() const;
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

protected:
    void run() override;

private:
    // --- Scheduler constants (spec §3) ------------------------------------
    static constexpr int kLeadMs = 500;            // video window ahead of P (travel dir)
    static constexpr int kTrailMs = 300;           // video window behind P
    static constexpr int kChunkMs = 500;           // reverse backward-fetch chunk size
    static constexpr int kAudioLeadMs = 200;       // max lead of pushed audio over P
    static constexpr int kAudioQueueMs = 900;      // worker audio-queue span bound
    static constexpr int kSlackMs = 200;           // trim hysteresis beyond the window
    static constexpr int kIdleSleepMs = 3;         // sleep when window full and playing
    static constexpr int kEofSleepMs = 10;         // sleep between EOF re-checks
    static constexpr int kReadErrSleepMs = 20;     // sleep after a non-EOF read error
    static constexpr int kBackJumpSlackMs = 150;   // P below buffered span by this ⇒ reposition
    static constexpr int kGlobalFrameBudget = 256; // aggregate decoded-frame cap (memory)
    static constexpr double kDecimateAbove = 1.5;  // |speed| above which decimation engages

    // --- Tier3 pre-roll / armed-cut constants -----------------------------
    static constexpr int kStagingSpanMs = 800;       // window staged ahead of target
    static constexpr int kPrerollPacketsPerTick = 8; // bounded per run() iter (no starve)
    static constexpr int kCutLeadMs = 120;           // lead before the cut fires (output frames)
    static constexpr int kPrerollAudioSpanMs = 800;  // active-view audio staged ahead of a cut

    // High-performance conversion from FFmpeg AVFrame to backend YUV420P media frames.
    MediaVideoFrame convertToMediaVideoFrame(AVFrame* frame, int feedIndex);

    // --- Scheduler helpers (spec §3 symbols / §6). Task 5 wires the loop;
    //     bodies are implemented here except repositionTo (stubbed). ---------
    int fps() const;            // m_transport->fps(), clamped >=1
    int64_t frameDurMs() const; // 1000 / fps()
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
    bool reuseAt(int64_t target); // true if every track has a frame within frameDurMs/2

    // Decode one read packet into the bank (video → insert with cap; audio →
    // enqueue active view). Used by forward fill, reposition, and reverse fill.
    // `decimate` engages count-based decimation; `audioOn` gates audio enqueue.
    // `dedupTail` skips video/audio packets at/behind the owning track's newest
    // (post-EOF-unlatch re-read guard, §6.8). Returns the just-decoded video
    // PTS (ms) of the last frame produced, or INT64_MIN if none.
    int64_t decodePacketIntoBank(AVPacket* pkt, AVFrame* vf, AVFrame* af, int64_t P, int dir,
                                 int trackCount, bool decimate, int decimateStep, bool audioOn,
                                 bool dedupTail);
    // Enqueue a decoded active-view audio frame onto m_audioQueue (format-guarded).
    void enqueueAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame, bool dedupTail);
    void cacheOutputAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame, bool dedupTail);
    void resetDedup();          // lastDeliveredPtsMs = -1 on every track
    void clearDecoderBuffers(); // clear every TrackBuffer (holds m_bufferMutex); leaves
                                // m_outputCache intact
    void initializeOutputGraph(int feedCount, int width, int height);
    void shutdownOutputGraph();
    void rebuildOutputEndpoints();
    OutputRuntimeSnapshot makeOutputSnapshot() const;
    // Snapshot m_outputCache into the published immutable slot. Caller must hold
    // m_bufferMutex.
    void publishOutputCacheLocked();

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
    void armCutInternal(int64_t targetMs, uint64_t baselineSeekGen);
    // Store the atomic schedule (output frame index + target ms).
    void scheduleCutAtFrame(qint64 outputFrameIndex, int64_t targetMs);
    // Fire the scheduled cut iff the dispatcher's next index reached it: swaps
    // staging -> active, republishes, re-bases the transport playhead. MUST be
    // called holding m_bufferMutex (invoked from makeOutputSnapshot).
    void maybeFireScheduledCut(qint64 dispatcherNextIndex);

    static int ffmpegInterruptCallback(void* opaque);
    bool shouldInterrupt() const;

    QList<FrameProvider*> m_providers;
    FrameProvider* m_multiviewPreviewProvider = nullptr;
    FrameProvider* m_pgmPreviewProvider = nullptr;
    QVector<DecoderTrack*> m_decoderBank;
    QVector<AudioDecoderTrack*> m_audioDecoderBank;
    AVFormatContext* m_fmtCtx = nullptr;

    std::atomic<bool> m_running{false};
    int64_t m_seekTargetMs = -1;
    QString m_currentFilePath;
    PlaybackTransport* m_transport;
    AudioPlayer* m_audioPlayer = nullptr;
    std::atomic<int> m_activeAudioView{-1};
    std::atomic<int> m_selectedOutputFeed{-1};

    AudioFrameQueue m_audioQueue;            // worker-thread-only
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
    // packets are read (worker-thread-only; no mutex). All recordings are
    // ALL-INTRA, so any indexed offset is a valid standalone decode start; the
    // full-reposition path avio_seeks straight to nearestAtOrBefore(target)
    // instead of the coarse av_seek_frame anchor, shortening the forward fill.
    // Survives clearDecoderBuffers (only the per-track frame buffers are wiped).
    FrameIndex m_frameIndex;

    // Seek-gate generations (read in makeOutputSnapshot; written in seekTo /
    // repositionTo). When m_committedGeneration == m_seekGeneration there is no
    // reposition outstanding and the live playhead is exposed (1x advances);
    // while they differ a seek is in flight against a not-yet-ready cache and
    // the gate holds m_committedPlayheadMs (CommitGate).
    std::atomic<uint64_t> m_seekGeneration{0};
    std::atomic<uint64_t> m_committedGeneration{0};
    std::atomic<int64_t> m_committedPlayheadMs{0};

    QMutex m_mutex;
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
    // Newest video PTS (ms) currently staged for the reference feed (feed 0),
    // tracked during fillStaging since OutputFrameCache has no newest accessor.
    int64_t m_stagingNewestRefPtsMs = INT64_MIN;
    // Scheduled cut: the output frame index to fire at + the target ms. Read in
    // maybeFireScheduledCut (under m_bufferMutex); written by scheduleCutAtFrame.
    std::atomic<qint64> m_scheduledCutFrame{-1};
    std::atomic<int64_t> m_scheduledCutTargetMs{-1};
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
    // m_seekGeneration captured when a cut is armed (armCutInternal). A manual
    // seekTo bumps m_seekGeneration; if it differs at fire time the operator
    // issued an explicit seek after arming, so maybeFireScheduledCut ABORTS the
    // cut (the manual seek wins — it services the jump via repositionTo). This is
    // the authoritative manual-seek-vs-in-flight-cut policy: the cut never fires
    // against a playhead the operator has since seeked away from.
    std::atomic<uint64_t> m_armSeekGen{0};
    // Immutable snapshot of m_outputCache published to the output thread
    // (replaces the per-tick deep copy in makeOutputSnapshot).
    SharedCacheSlot m_publishedCache;
    std::unique_ptr<OutputRuntime> m_outputRuntime;
    std::vector<std::unique_ptr<IOutputSink>> m_outputSinks;
    int m_outputFeedCount = 0;
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;

    PlaybackCounters m_counters;
    void emitTelemetry(int64_t P, int64_t newest, double speed);
};

#endif // PLAYBACKWORKER_H
