#ifndef PLAYBACKWORKER_H
#define PLAYBACKWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QVector>
#include <QMutex>
#include <QVideoFrame>
#include <QList>
#include <atomic>
#include "frameprovider.h"
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
    TrackBuffer buffer;
    int64_t lastDeliveredPtsMs = -1;   // last frame released to the provider
    int decimateCounter = 0;           // per-track keep-counter (§6.3 decimation)
};

struct AudioDecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    int streamIndex = -1;
    int viewIndex = -1;             // which view (0..N-1) this audio belongs to
    int64_t lastEnqueuedPtsMs = -1; // for dedup-before-decode after EOF un-latch
};

class PlaybackWorker : public QThread {
    Q_OBJECT
public:
    struct PlaybackCounters {
        int reposition = 0, reuseSeek = 0, reverseChunkSeek = 0, eofTailSeek = 0, skipForward = 0,
            audioPushes = 0, framesDropped = 0;
    };

    explicit PlaybackWorker(const QList<FrameProvider*> &providers, PlaybackTransport *transport,
                            AudioPlayer *audioPlayer = nullptr, QObject *parent = nullptr);
    ~PlaybackWorker();

    void openFile(const QString &filePath);
    void seekTo(int64_t timestampMs);
    // Direction-aware delivery (spec §5): forward delivers iff pts moved up,
    // reverse iff pts moved down (dir = +1 / -1).
    void deliverDueFrames(int64_t P, int dir);
    void setActiveAudioView(int viewIndex);
    void stop();

    PlaybackCounters counters() const { return m_counters; }

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

    // High-performance conversion from FFmpeg AVFrame to QVideoFrame (YUV420P)
    QVideoFrame convertToQVideoFrame(AVFrame* frame);

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
    void resetDedup();      // lastDeliveredPtsMs = -1 on every track
    void clearAllBuffers(); // clear every TrackBuffer (holds m_bufferMutex)

    static int ffmpegInterruptCallback(void* opaque);
    bool shouldInterrupt() const;

    QList<FrameProvider*> m_providers;
    QVector<DecoderTrack*> m_decoderBank;
    QVector<AudioDecoderTrack*> m_audioDecoderBank;
    AVFormatContext* m_fmtCtx = nullptr;

    std::atomic<bool> m_running{false};
    int64_t m_seekTargetMs = -1;
    QString m_currentFilePath;
    PlaybackTransport *m_transport;
    AudioPlayer *m_audioPlayer = nullptr;
    std::atomic<int> m_activeAudioView{-1};

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

    QMutex m_mutex;
    mutable QMutex m_bufferMutex;

    PlaybackCounters m_counters;
    void emitTelemetry(int64_t P, int64_t newest, double speed);
};

#endif // PLAYBACKWORKER_H
