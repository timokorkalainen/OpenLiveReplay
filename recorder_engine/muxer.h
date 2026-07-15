#ifndef MUXER_H
#define MUXER_H

#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "recorder_engine/codec/videocodecchoice.h"

class Muxer {
public:
    static constexpr size_t kMaxQueuedPackets = 4096;
    struct PacketWriteCallback {
        using Function = void (*)(void* context, uint64_t id, bool written);

        void* context = nullptr;
        uint64_t id = 0;
        Function function = nullptr;

        void operator()(bool written) const {
            if (function) function(context, id, written);
        }
        explicit operator bool() const noexcept { return function != nullptr; }

        template <typename Callable>
        static PacketWriteCallback bind(Callable& callable) noexcept {
            return PacketWriteCallback{&callable, 0, [](void* context, uint64_t, bool written) {
                                           (*static_cast<Callable*>(context))(written);
                                       }};
        }
    };
    static_assert(std::is_trivially_copyable_v<PacketWriteCallback>);
    struct PacketCarrierGuard {
        using Function = bool (*)(void* context, uint64_t sessionIdentity, uint64_t epoch);

        PacketCarrierGuard() noexcept
            : context(nullptr), sessionIdentity(0), epoch(0), function(nullptr),
              currentEpoch(nullptr) {}
        PacketCarrierGuard(const std::atomic<uint64_t>* currentEpoch_,
                           uint64_t expectedEpoch) noexcept
            : context(nullptr), sessionIdentity(0), epoch(expectedEpoch), function(nullptr),
              currentEpoch(currentEpoch_) {}
        PacketCarrierGuard(void* context_, uint64_t sessionIdentity_, uint64_t epoch_,
                           Function function_) noexcept
            : context(context_), sessionIdentity(sessionIdentity_), epoch(epoch_),
              function(function_), currentEpoch(nullptr) {}

        void* context;
        uint64_t sessionIdentity;
        uint64_t epoch;
        Function function;
        const std::atomic<uint64_t>* currentEpoch;
        bool accepts() const noexcept {
            if (function) return function(context, sessionIdentity, epoch);
            return !currentEpoch || epoch == 0 ||
                   currentEpoch->load(std::memory_order_acquire) == epoch;
        }
    };
    static_assert(std::is_trivially_copyable_v<PacketCarrierGuard>);

    Muxer();
    ~Muxer();

    // fpsNum/fpsDen: the rational frame rate advertised on each video stream's
    // avg_frame_rate / r_frame_rate. 0/0 (the default) keeps the legacy {fps, 1};
    // pass e.g. 30000/1001 so a 29.97 recording advertises its true rate. The
    // integer `fps` still drives defaults and the ms time_base (PTS is unchanged).
    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
              const QStringList& streamNames, int audioSampleRate = 48000, int audioChannels = 2,
              VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software,
              const QByteArray& videoExtradata = {}, const QString& startTimecode = QString(),
              int fpsNum = 0, int fpsDen = 0);
    // Convenience overload carrying ONLY the session start timecode (default
    // codec). startTimecode is REQUIRED here (no default) so this 9-arg form is
    // distinct from the codec-tail overload above (whose 9th positional arg is a
    // VideoCodecChoice, not a QString): an 8-arg call still resolves to the
    // codec-tail overload, a 9-arg call with a QString resolves here. When
    // startTimecode is a valid "HH:MM:SS[:;]FF" it is written as the standard
    // FFmpeg "timecode" tag on the output and each video track; empty or
    // malformed -> no tag (a no-TC recording is byte-identical to before).
    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
              const QStringList& streamNames, int audioSampleRate, int audioChannels,
              const QString& startTimecode, int fpsNum = 0, int fpsDen = 0);
    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
              const QStringList& streamNames, const QStringList& telemetryFeedIds,
              const QStringList& telemetryFeedNames, int audioSampleRate = 48000,
              int audioChannels = 2, VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software,
              const QByteArray& videoExtradata = {}, const QString& startTimecode = QString(),
              int fpsNum = 0, int fpsDen = 0);
    // Returns true when the packet was accepted into the writer queue. The optional
    // callback still reports the later disk-write result.
    bool writePacket(AVPacket* pkt,
                     PacketWriteCallback onWritten = PacketWriteCallback{nullptr, 0, nullptr},
                     const QString& startTimecodeCandidate = QString(),
                     PacketCarrierGuard carrierGuard = PacketCarrierGuard{});
    bool writeMetadataPacket(int viewTrack, int64_t ptsMs, const QByteArray& jsonData);
    bool writeTelemetryPacket(int feedIndex, int64_t ptsMs, const QByteArray& jsonData);
    void beginShutdownDrain();
    // Publish an already-accepted session-start timecode candidate to the deferred
    // header. Per-packet producers pass candidates to writePacket(), which chooses
    // the winner under m_qMutex in queue-acceptance order. This method is retained
    // for explicit/up-front callers and for the writer's publication step. Empty or
    // malformed candidates are ignored; m_headerMutex makes it thread-safe.
    void setStartTimecodeCandidate(const QString& tc);
    AVStream* getStream(int index);
    void close();

    bool hasFatalWriteError() const { return m_fatalWriteError.load(std::memory_order_acquire); }
    QString fatalWriteMessage() const {
        std::lock_guard<std::mutex> lk(m_fatalMsgMutex);
        return QString::fromStdString(m_fatalWriteMsg);
    }

    int audioTrackOffset() const { return m_audioTrackOffset; }
    int subtitleTrackOffset() const { return m_subtitleTrackOffset; }
    int telemetryTrackOffset() const { return m_telemetryTrackOffset; }
    int64_t minWrittenVideoPtsMs() const;

    QString getVideoPath(QString fileName);

    // Directory recordings are written to.  Set BEFORE init()/getVideoPath()
    // from the main thread; empty = default (~/Documents/videos).
    // Deliberately unlocked: init() calls getVideoPath() while holding
    // m_mutex, and the value never changes during a recording session.
    void setOutputDirectory(const QString& dir) { m_outputDir = dir; }

private:
    // Drains m_pktQueue and performs the actual av_write_frame/avio_flush.
    // Runs on m_writerThread; the ONLY thread that touches m_outCtx between
    // init() and close(), so the write path needs no lock against the
    // AVFormatContext.
    void writerLoop();

    // Records a single write outcome and drives the consecutive-failure latch.
    // Called ONLY from the writer thread; no lock needed for the counter.
    // failed==true: increment counter; on reaching kFatalWriteThreshold, set the
    // fatal flag (once). failed==false: reset the counter to 0.
    void recordWriteOutcome(bool failed, const char* errLabel);
    void normalizePacketDts(AVPacket* pkt);
    void rememberWrittenPacketDts(const AVPacket* pkt);
    void rememberWrittenPacketPts(const AVPacket* pkt);

    // Writes the deferred MKV header exactly once, materialising the winning
    // start-timecode candidate into the "timecode" tag (format-level + each video
    // track) at that moment. Idempotent and thread-safe (m_headerMutex). Returns
    // true once the header is (or already was) written; false if the underlying
    // avformat_write_header failed. Called by the writer thread before draining the
    // first queued packet, and from close() so an empty recording still gets a header.
    //
    // LOCK ORDERING: no path holds m_headerMutex while acquiring m_qMutex. The
    // writer snapshots accepted candidates under m_qMutex, releases it, and only
    // then publishes/writes the header. close() takes m_mutex, then (via
    // ensureHeaderWritten) m_headerMutex; ensureHeaderWritten never reaches back
    // for m_mutex, so there is no cycle.
    bool ensureHeaderWritten();
    bool publishStartTimecodeCandidate(const QString& tc, uint64_t publicationId);
    void undoStartTimecodeCandidatePublication(const QString& tc, uint64_t publicationId);

    // True while the header write should be HELD for the first source timecode:
    // unwritten header + no winning candidate yet + grace window still open. The
    // writer thread polls this and keeps the popped packet (no drop, no reorder)
    // until it returns false, then commits via ensureHeaderWritten(). Thread-safe
    // (m_headerMutex). See the m_headerGrace* fields for the rationale.
    bool headerWriteDeferred();

    QString m_outputDir;
    // Path resolved by init() for the current session; getVideoPath()
    // returns it while recording so the reader can never diverge from
    // the file actually being written.
    QString m_activePath;
    AVFormatContext* m_outCtx = nullptr;
    // Last DTS per stream (monotonicity enforcement). Touched ONLY by the
    // writer thread (writerLoop), so it needs no lock.
    QHash<int, int64_t> m_lastDts;
    // Throttles avio_flush: flushing per packet hammers the disk for no
    // benefit beyond chase-play visibility (~100 ms is plenty). Touched ONLY
    // by the writer thread.
    QElapsedTimer m_lastFlush;
    // Guards init()/close() and getVideoPath() against each other. The write
    // path no longer takes this — av_write_frame runs on the writer thread.
    QMutex m_mutex;
    bool m_initialized = false;

    // ─── Deferred header write (timecode-on-first-packet) ─────────────────────
    // The MKV header is written on the first muxed packet, not in init(), so the
    // session start timecode (the first muxed frame's TC) can be captured into the
    // "timecode" tag — live recordings observe no TC at start. m_headerMutex guards
    // all three fields and serialises the one-time avformat_write_header.
    // ensureHeaderWritten never reaches for another Muxer lock while holding it
    // (see ensureHeaderWritten doc for ordering).
    QMutex m_headerMutex;
    bool m_headerWritten = false;
    QString m_startTimecodeCandidate;
    uint64_t m_startTimecodeCandidatePublicationId = 0;
    // Bounded "wait for the first source TC" grace. A live recording observes no
    // TC at start and emits BLUE/pre-connect packets (TC=-1) before the first real
    // source frame carrying a timecode. Committing the header on that first no-TC
    // packet would lose the tmcd tag forever (first-candidate-wins, header-once).
    // So ensureHeaderWritten() DEFERS the header write for a small grace window
    // after init while no candidate has been registered yet; it commits early the
    // instant a well-formed candidate arrives, or unconditionally once the grace
    // expires (a no-TC recording then writes the header with no tag, byte-identical
    // content, only the header flush moves a few hundred ms later). The grace timer
    // starts in init(); the writer thread (writerLoop) honours the deferral by
    // keeping the popped packet and retrying, so NO packet is ever dropped/reordered.
    QElapsedTimer m_headerGraceTimer;
    int m_headerGraceMs = 0;
    // Matroska muxer options (reserve_index_space/cluster/live), built in init()
    // and consumed by the deferred avformat_write_header in ensureHeaderWritten.
    AVDictionary* m_headerOpts = nullptr;

    int m_audioTrackOffset = 0;     // Index of first audio track
    int m_subtitleTrackOffset = 0;  // Index of first subtitle track
    int m_telemetryTrackOffset = 0; // Index of first per-feed telemetry track
    int m_telemetryTrackCount = 0;
    int m_videoTrackCount = 0;
    std::vector<int64_t> m_lastWrittenVideoPtsMs;
    mutable std::mutex m_writtenPtsMutex;

    // ─── Dedicated writer thread (decouples callers from the disk) ─────────
    // writePacket() enqueues a cloned packet and returns immediately; the
    // writer thread drains the queue and performs the blocking disk writes,
    // so worker tick threads and the GUI thread never block on a stalled disk
    // (except, by design, when a sustained stall fills the bounded queue).
    struct QueuedPacket {
        AVPacket* pkt = nullptr;
        PacketWriteCallback onWritten;
        PacketCarrierGuard carrierGuard;
        uint64_t sequence = 0;
    };
    struct AcceptedCandidate {
        QString value;
        PacketCarrierGuard carrierGuard;
        uint64_t sequence = 0;
    };

    std::thread m_writerThread;
    std::queue<QueuedPacket> m_pktQueue; // owns the cloned packets it holds
    uint64_t m_nextQueuedPacketSequence = 1;
    std::mutex m_qMutex;
    std::condition_variable m_qCv;
    // First valid candidate attached to a packet actually accepted into m_pktQueue.
    // Guarded by m_qMutex so concurrent producers resolve in queue-acceptance order.
    QString m_acceptedStartTimecodeCandidate;
    std::deque<AcceptedCandidate> m_acceptedStartTimecodeCandidates;
    uint64_t m_candidateWindowGeneration = 1;
    bool m_startTimecodeCandidateWindowClosed = false;
    std::atomic<bool> m_writerRunning{false};
    std::atomic<bool> m_blockingWritesAllowed{true};

    // Set on the FIRST sustained write failure (kFatalWriteThreshold consecutive
    // av_write_frame errors on any stream). Written once; reset only on init().
    // m_consecutiveWriteErrors is touched ONLY on the writer thread: plain int.
    int m_consecutiveWriteErrors = 0;
    std::atomic<bool> m_fatalWriteError{false};
    std::string m_fatalWriteMsg; // guarded by m_fatalMsgMutex
    mutable std::mutex m_fatalMsgMutex;
    static constexpr int kFatalWriteThreshold = 3;

#ifdef OLR_UNIT_TEST
    friend class TestMuxer;
    std::function<void()> m_afterCandidateSnapshotForTest;
    std::function<void()> m_beforeCandidatePublicationForTest;
#endif
};

#endif // MUXER_H
