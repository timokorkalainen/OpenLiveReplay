#ifndef STREAMWORKER_H
#define STREAMWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QString>
#include <QFuture>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <atomic>

#include "recordingclock.h"
#include "muxer.h"

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/avutil.h>
    #include <libavutil/time.h>
    #include <libavutil/error.h>
    #include <libswscale/swscale.h>
}

class StreamWorker : public QThread {
    Q_OBJECT
public:
    // sourceIndex: fixed identity of this source (for logging)
    // initialViewTrack: which muxer track to encode into (-1 = no view assigned)
    StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock* clock,
                 int targetWidth, int targetHeight, int targetFps, QObject* parent = nullptr);
    ~StreamWorker();

    // Change the source URL (real FFmpeg reconnect — only for user editing a URL)
    void changeSource(const QString& newUrl);

    // Atomically set which muxer view-track this source writes to.
    // -1 = not assigned to any view (still captures, just doesn't encode).
    void setViewTrack(int track) { m_viewTrack.store(track, std::memory_order_relaxed); }
    int viewTrack() const { return m_viewTrack.load(std::memory_order_relaxed); }

    void stop();

    int sourceIndex() const { return m_sourceIndex; }

public slots:
    void onMasterPulse(int64_t frameIndex, int64_t streamTimeMs);

protected:
    void run() override;

private:
    QString m_url;
    int m_sourceIndex;              // Fixed: identity of this source
    std::atomic<int> m_viewTrack;   // Dynamic: muxer track to write to (-1 = none)
    Muxer* m_muxer;

    AVFrame* m_latestFrame = nullptr;
    int64_t m_internalFrameCount;
    RecordingClock* m_sharedClock;

    QAtomicInt m_restartCapture;    // Thread-safe flag to signal a source swap
    QAtomicInt m_paintBlue{0};      // Deferred blue-paint flag

    //Mutexes & Threads
    QMutex m_frameMutex;
    QMutex m_urlMutex;
    QFuture<void> m_captureFuture;

    void captureLoop();
    std::atomic<bool> m_captureRunning{false};
    QElapsedTimer m_lastPacketTimer;
    QElapsedTimer m_lastFrameEnqueueTimer;
    int m_stallTimeoutMs = 8000;
    std::atomic<bool> m_connected{false};
    int m_connectBackoffMs = 1000;
    int m_jitterBufferMs = 200;

    int m_targetWidth = 1920;
    int m_targetHeight = 1080;
    int m_targetFps = 30;

    static int ffmpegInterruptCallback(void* opaque);
    bool shouldInterrupt() const;

    // FFmpeg context management
    struct QueuedFrame {
        AVFrame* frame;
        int64_t sourcePts;
    };

    struct SwsContext* m_swsCtx = nullptr;
    QQueue<QueuedFrame> m_frameQueue;
    AVCodecContext* m_persistentEncCtx = nullptr;

    // FFmpeg helpers
    bool setupDecoder(AVFormatContext** inCtx, AVCodecContext** decCtx, QUrl url, int* videoStreamIdx);
    bool setupEncoder(AVCodecContext** encCtx);
    void processEncoderTick(AVCodecContext *encCtx, int64_t streamTimeMs);
};

#endif // STREAMWORKER_H
