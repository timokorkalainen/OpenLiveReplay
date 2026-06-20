#ifndef MUXER_H
#define MUXER_H

#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

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
    Muxer();
    ~Muxer();

    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
              const QStringList& streamNames, int audioSampleRate = 48000, int audioChannels = 2,
              VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software,
              const QByteArray& videoExtradata = {}, const QString& startTimecode = QString());
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
              const QString& startTimecode);
    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps,
              const QStringList& streamNames, const QStringList& telemetryFeedIds,
              const QStringList& telemetryFeedNames, int audioSampleRate = 48000,
              int audioChannels = 2, VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software,
              const QByteArray& videoExtradata = {}, const QString& startTimecode = QString());
    void writePacket(AVPacket* pkt);
    void writeMetadataPacket(int viewTrack, int64_t ptsMs, const QByteArray& jsonData);
    void writeTelemetryPacket(int feedIndex, int64_t ptsMs, const QByteArray& jsonData);
    // Offer a session-start timecode candidate. The header is written on the FIRST
    // muxed packet (see ensureHeaderWritten); the FIRST well-formed candidate
    // registered before that wins and becomes the file's "timecode" tag. Empty or
    // malformed candidates are ignored. Thread-safe: called from every worker tick
    // thread; guarded by m_headerMutex. A no-op once the header is written.
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

    // Writes the deferred MKV header exactly once, materialising the winning
    // start-timecode candidate into the "timecode" tag (format-level + each video
    // track) at that moment. Idempotent and thread-safe (m_headerMutex). Returns
    // true once the header is (or already was) written; false if the underlying
    // avformat_write_header failed. Called at the TOP of every write path before
    // any queue lock, and from close() so an empty recording still gets a header.
    //
    // LOCK ORDERING: m_headerMutex is the FIRST lock taken on any write — it is
    // never held while acquiring m_qMutex (writePacket releases it implicitly by
    // returning from ensureHeaderWritten before locking the queue). close() takes
    // m_mutex, then (via ensureHeaderWritten) m_headerMutex; ensureHeaderWritten
    // never reaches back for m_mutex, so there is no cycle.
    bool ensureHeaderWritten();

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
    // all three fields and serialises the one-time avformat_write_header. It is the
    // FIRST lock on any write path; ensureHeaderWritten never reaches for another
    // Muxer lock while holding it (see ensureHeaderWritten doc for ordering).
    QMutex m_headerMutex;
    bool m_headerWritten = false;
    QString m_startTimecodeCandidate;
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

    // ─── Dedicated writer thread (decouples callers from the disk) ─────────
    // writePacket() enqueues a cloned packet and returns immediately; the
    // writer thread drains the queue and performs the blocking disk writes,
    // so worker tick threads and the GUI thread never block on a stalled disk
    // (except, by design, when a sustained stall fills the bounded queue).
    static constexpr size_t kMaxQueued = 4096; // ~ a few seconds of packets
    std::thread m_writerThread;
    std::queue<AVPacket*> m_pktQueue; // owns the cloned packets it holds
    std::mutex m_qMutex;
    std::condition_variable m_qCv;
    std::atomic<bool> m_writerRunning{false};

    // Set on the FIRST sustained write failure (kFatalWriteThreshold consecutive
    // av_write_frame errors on any stream). Written once; reset only on init().
    std::atomic<bool> m_fatalWriteError{false};
    std::string m_fatalWriteMsg; // guarded by m_fatalMsgMutex
    mutable std::mutex m_fatalMsgMutex;
    static constexpr int kFatalWriteThreshold = 3;

#ifdef OLR_UNIT_TEST
    friend class TestMuxer;
#endif
};

#endif // MUXER_H
