#ifndef STREAMWORKER_H
#define STREAMWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QString>
#include <QFuture>
#include <QtConcurrent>
#include <atomic>

#include "recordingclock.h"
#include "muxer.h"

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/time.h>
    #include <libswscale/swscale.h>
}

class IngestWorker : public QObject {
    Q_OBJECT
public:
    void process(AVFormatContext* inCtx, AVCodecContext* decCtx, int videoIdx,
                 AVFrame* rawFrame, AVFrame* latestFrame,
                 struct SwsContext** swsCtx, QMutex* mutex);
};

class StreamWorker : public QThread {
    Q_OBJECT
public:
    StreamWorker(const QString& url, int trackIndex, Muxer* muxer, RecordingClock* clock, QObject* parent = nullptr);
    ~StreamWorker();

    void changeSource(const QString& newUrl);

    void stop();

public slots:
    void onMasterPulse(int64_t frameIndex, int64_t streamTimeMs);

protected:
    void run() override;

private:
    QString m_url;
    int m_trackIndex;
    Muxer* m_muxer;

    AVFrame* m_latestFrame = nullptr;
    int64_t m_internalFrameCount;
    RecordingClock* m_sharedClock;

    QAtomicInt m_restartCapture; // Thread-safe flag to signal a source swap

    //Mutexes & Threads
    QMutex m_frameMutex;
    QMutex m_urlMutex;
    QFuture<void> m_captureFuture;

    QThread* m_ingestThread = nullptr;
    void captureLoop();
    std::atomic<bool> m_captureRunning{false};

    // FFmpeg context management
    struct QueuedFrame {
        AVFrame* frame;
        int64_t sourcePts; // The original timestamp from the camera
    };

    struct SwsContext* m_swsCtx = nullptr;
    AVFrame* m_scaledFrame = nullptr;
    QQueue<QueuedFrame> m_frameQueue;
    AVCodecContext* m_persistentEncCtx;

    // FFmpeg helpers
    bool setupDecoder(AVFormatContext** inCtx, AVCodecContext** decCtx, QUrl url);
    bool setupEncoder(AVCodecContext** encCtx);
    void processEncoderTick(AVCodecContext *encCtx, int64_t streamTimeMs);
};

#endif // STREAMWORKER_H
