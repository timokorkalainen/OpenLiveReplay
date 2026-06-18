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
#include "playback/output/outputruntime.h"
#include "playback/output/sharedcacheslot.h"
#include "playback/output/outputtargetassignment.h"
#include "playback/playbacktransport.h"
#include "playback/audioplayer.h"
#include "playback/trackbuffer.h"
#include "playback/audioframequeue.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

struct DecoderTrack {
    AVCodecContext* codecCtx = nullptr;
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
    };

    explicit PlaybackWorker(const QList<FrameProvider*>& providers, PlaybackTransport* transport,
                            AudioPlayer* audioPlayer = nullptr, QObject* parent = nullptr);
    ~PlaybackWorker();

    void openFile(const QString& filePath);
    void seekTo(int64_t timestampMs);
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
    void repositionTo(int64_t target, int dir, AVPacket* pkt, AVFrame* vf, AVFrame* af);
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
