#ifndef STREAMWORKER_H
#define STREAMWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QString>
#include <QElapsedTimer>
#include <QMutex>
#include <QQueue>
#include <QByteArray>
#include <QUrl>
#include <atomic>
#include <thread>

#include "recordingclock.h"
#include "muxer.h"

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/avutil.h>
    #include <libavutil/time.h>
    #include <libavutil/error.h>
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
}

class StreamWorker : public QThread {
    Q_OBJECT
public:
    // Delay applied to captured media before it is written to the file.
    // Video frames sit in the jitter queue this long; audio shares the
    // same delay so both land on the same timeline.
    static constexpr int kJitterBufferMs = 200;

    // Recorded audio format (48 kHz stereo S16, conformed by swresample)
    static constexpr int kAudioSampleRate = 48000;
    static constexpr int kAudioBytesPerSample = 2 * int(sizeof(int16_t));

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

    // Per-source metadata JSON blob written to the subtitle track each frame
    void setSourceMetadata(const QByteArray& json) {
        QMutexLocker locker(&m_metadataMutex);
        m_sourceMetadataJson = json;
    }

    void stop();

    int sourceIndex() const { return m_sourceIndex; }

signals:
    // Emitted from the capture thread ONLY when the connection state flips
    // (debounced via setConnected). Cross-thread: relayed to the UI through
    // ReplayManager with a queued connection.
    void connectionChanged(int sourceIndex, bool connected);

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

    // Set when the source is changed to an empty URL (blue-paint state),
    // cleared when a non-empty URL actually connects.  While set, the
    // capture thread refuses to enqueue frames so a late straggler decoded
    // from the old/cleared source cannot overwrite the painted blue frame.
    std::atomic<bool> m_suppressEnqueue{false};

    //Mutexes & Threads
    QMutex m_frameMutex;
    QMutex m_urlMutex;
    QMutex m_metadataMutex;
    QByteArray m_sourceMetadataJson;    // JSON blob for per-frame subtitle track

    // Dedicated capture thread owned by this worker.  captureLoop() loops
    // internally on reconnect/URL-change (m_restartCapture), so it is
    // started exactly once and joined on shutdown.  We do NOT use the
    // shared global QtConcurrent pool: an infinite captureLoop per source
    // would saturate it (maxThreadCount == core count), starving extra
    // sources and the async network-close tasks.
    std::thread m_captureThread;

    void captureLoop();
    std::atomic<bool> m_captureRunning{false};
    // Monotonic clock started once; activity stamps are atomics because
    // they are written by the capture thread and read by the thread that
    // delivers the master pulse and by the ffmpeg interrupt callback.
    // -1 = "not yet valid".
    QElapsedTimer m_monotonic;
    std::atomic<int64_t> m_lastPacketAtMs{-1};
    std::atomic<int64_t> m_lastFrameEnqueueAtMs{-1};
    int m_stallTimeoutMs = 8000;
    std::atomic<bool> m_connected{false};
    int m_connectBackoffMs = 1000;
    // Atomically update m_connected and emit connectionChanged on a real
    // transition (false<->true). Called from the capture thread.
    void setConnected(bool c);

    // Last jitter-pull gate published by the tick thread (file-timeline ms,
    // -1 until the first tick).  The capture thread uses it to pre-drain
    // frames the next tick would discard anyway.
    std::atomic<int64_t> m_lastTickTargetMs{-1};

    // Audio FIFO: the capture thread produces resampled 48 kHz stereo S16
    // stamped on the global recording timeline; the master-pulse tick
    // consumes it on a sample-accurate cursor (gap-filled with silence).
    QMutex m_audioFifoMutex;
    QByteArray m_audioFifo;
    int64_t m_audioFifoStartSample = -1;  // timeline sample index of m_audioFifo[0]
    int64_t m_audioWriteCursor = -1;      // next sample to mux (tick thread only)
    void enqueueAudio(int64_t startSample, const uint8_t* data, int numSamples);
    void writeAudioForTick(int64_t recordingTimeMs, int track);

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
