#ifndef MUXER_H
#define MUXER_H

#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "framerate.h"

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

class Muxer {
public:
    Muxer();
    ~Muxer();

    bool init(const QString& filename, int videoTrackCount, int width, int height, FrameRate rate,
              const QStringList& streamNames, int audioSampleRate = 48000, int audioChannels = 2);
    bool init(const QString& filename, int videoTrackCount, int width, int height, FrameRate rate,
              const QStringList& streamNames, const QStringList& telemetryFeedIds,
              const QStringList& telemetryFeedNames, int audioSampleRate = 48000,
              int audioChannels = 2);
    void writePacket(AVPacket* pkt);
    void writeMetadataPacket(int viewTrack, int64_t ptsMs, const QByteArray& jsonData);
    void writeTelemetryPacket(int feedIndex, int64_t ptsMs, const QByteArray& jsonData);
    AVStream* getStream(int index);
    void close();

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
};

#endif // MUXER_H
