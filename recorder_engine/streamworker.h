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
#include "ingest/ingestsession.h"
#include "timing/sourceclock.h"

#include "recorder_engine/codec/videocodecchoice.h"
#include "recorder_engine/codec/nativevideoencoder.h"

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

    // SRT sources lean on SRT's TSBPD reorder buffer, so the engine only needs a
    // small residual window instead of the full kJitterBufferMs. Env-overridable
    // (OLR_SRT_JITTER_MS) for tuning/validation. Non-SRT transports keep 200.
    static constexpr int kSrtJitterFloorMs = 80;

    // Max magnitude of the per-source timeline trim (ms). +delay / -advance.
    static constexpr int kMaxTrimMs = 500;

    // Max magnitude of the inter-camera phase servo trim (ms). DELIBERATELY small:
    // it nudges a follower source toward the reference by a few frames, never fights
    // the operator trim, and never jerks the timeline. Driven by ReplayManager
    // (Phase 4), summed with the operator trim once per pulse.
    static constexpr int kMaxServoTrimMs = 80;

    // Recorded audio format (48 kHz stereo S16, conformed by swresample)
    static constexpr int kAudioSampleRate = 48000;
    static constexpr int kAudioBytesPerSample = 2 * int(sizeof(int16_t));

    // sourceIndex: fixed identity of this source (for logging)
    // initialViewTrack: which muxer track to encode into (-1 = no view assigned)
    StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock* clock,
                 int targetWidth, int targetHeight, int targetFps,
                 VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software, QObject* parent = nullptr);
    ~StreamWorker();

    // Change the source URL (real FFmpeg reconnect — only for user editing a URL)
    void changeSource(const QString& newUrl);

    // Atomically set which muxer view-track this source writes to.
    // -1 = not assigned to any view (still captures, just doesn't encode).
    void setViewTrack(int track) { m_viewTrack.store(track, std::memory_order_relaxed); }
    int viewTrack() const { return m_viewTrack.load(std::memory_order_relaxed); }

    // Per-source timeline trim in ms (+delay / -advance), clamped to ±kMaxTrimMs.
    // Read once per pulse, so it takes effect live. NOTE: increasing the trim
    // mid-recording briefly silences/repeats up to the trim delta while the
    // audio FIFO accumulates the newly-needed history; a set-and-leave trim is
    // unaffected.
    void setTrimOffsetMs(int ms) {
        m_trimOffsetMs.store(qBound(-kMaxTrimMs, ms, kMaxTrimMs), std::memory_order_relaxed);
    }

    // Inter-camera phase servo trim in ms (+delay / -advance), SEPARATE from the
    // operator trim above and clamped to ±kMaxServoTrimMs. Driven by ReplayManager's
    // bounded phase servo (Phase 4). Snapshotted alongside the operator trim once per
    // pulse (onMasterPulse) and SUMMED into the same jitter-pull target, so operator +
    // servo compose into one offset and stay A/V-locked. 0 (the default) is
    // byte-identical to today.
    void setServoTrimOffsetMs(int ms) {
        m_servoTrimOffsetMs.store(qBound(-kMaxServoTrimMs, ms, kMaxServoTrimMs),
                                  std::memory_order_relaxed);
    }

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

    // Emitted ~1/sec from the capture thread with the source's latest ingest stats
    // (native SRT or RTMP; tagged by IngestStats::kind). Cross-thread: relayed to the
    // UI through ReplayManager with a queued connection, like connectionChanged.
    void statsUpdated(int sourceIndex, IngestStats stats);

    // Emitted from the tick thread when a frame carrying a valid source timecode is
    // consumed for this source's assigned view track, with the session frame index
    // (m_internalFrameCount) it landed on. ONLY emitted when sourceTimecode100ns >= 0
    // — sources without TC never emit it, so behavior is unchanged when TC is absent.
    // ReplayManager feeds it into its TimecodeAligner (Qt::QueuedConnection).
    void frameTimecode(int sourceIndex, int64_t sourceTimecode100ns, int64_t sessionFrameIndex);

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
    // Source timecode (100 ns since midnight) of the frame currently held in
    // m_latestFrame, or -1 when none/blue. Tick-thread-only. Travels with the
    // frame through the jitter pull so the muxed frame's TC can be forwarded.
    int64_t m_latestFrameTimecode100ns = -1;
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
    QMutex m_sessionMutex;
    QByteArray m_sourceMetadataJson;    // JSON blob for per-frame subtitle track
    IngestSession* m_activeSession = nullptr; // guarded by m_sessionMutex

    // Dedicated capture thread owned by this worker.  captureLoop() loops
    // internally on reconnect/URL-change (m_restartCapture), so it is
    // started exactly once and joined on shutdown.  We do NOT use the
    // shared global QtConcurrent pool: an infinite captureLoop per source
    // would saturate it (maxThreadCount == core count), starving extra
    // sources and the async network-close tasks.
    std::thread m_captureThread;

    void captureLoop();
    std::atomic<bool> m_captureRunning{false};
    QElapsedTimer m_monotonic;
    std::atomic<int64_t> m_lastFrameEnqueueAtMs{-1};
    int m_stallTimeoutMs = 8000;
    std::atomic<bool> m_connected{false};
    // signed ms (+delay / -advance). Relaxed: standalone value, no associated
    // data to synchronize. Only setTrimOffsetMs() (clamped) writes it.
    std::atomic<int> m_trimOffsetMs{0};
    // Inter-camera phase servo trim (signed ms, +delay / -advance), SEPARATE from the
    // operator trim and clamped to ±kMaxServoTrimMs. Only setServoTrimOffsetMs() writes
    // it; summed with m_trimOffsetMs once per pulse in onMasterPulse. Default 0.
    std::atomic<int> m_servoTrimOffsetMs{0};
    // Per-source jitter window (ms), chosen by transport in captureLoop and read by
    // the tick thread. Defaults to kJitterBufferMs until the URL is resolved.
    std::atomic<int> m_activeJitterWindowMs{kJitterBufferMs};
    int m_connectBackoffMs = 1000;
    AnchoredSourceClock m_srtSourceClock{ClockQuality::Pcr, 90};
    AnchoredSourceClock m_rtmpSourceClock{ClockQuality::FlvPll};
    AnchoredSourceClock m_ndiSourceClock{ClockQuality::Ndi, 10000};
    QString m_clockOwnerUrl;
    IngestBackendKind m_clockOwnerBackend = IngestBackendKind::Unsupported;
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
    int64_t m_audioSourceCursor = -1;     // next source-timeline sample to consume
    int64_t m_audioServoTrimSamples = 0;
    int64_t m_audioServoJitterSamples = 0;
    void enqueueAudio(int64_t startSample, const uint8_t* data, int numSamples);
    void writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs, int64_t jitterMs);

    int m_targetWidth = 1920;
    int m_targetHeight = 1080;
    int m_targetFps = 30;
    VideoCodecChoice m_videoCodec = VideoCodecChoice::Mpeg2Software;

    // FFmpeg context management
    struct QueuedFrame {
        AVFrame* frame;
        int64_t sourcePts;
        // The frame's own source timecode (100 ns since midnight), or -1 when the
        // transport carried no TC. Purely additive: never affects A/V sync or the
        // jitter pull; only forwarded via frameTimecode() when the frame is muxed.
        int64_t sourceTimecode100ns = -1;
    };

    QQueue<QueuedFrame> m_frameQueue;
    AVCodecContext* m_persistentEncCtx = nullptr;
    std::unique_ptr<NativeVideoEncoder> m_nativeEncoder;

    // FFmpeg helpers
    bool setupEncoder(AVCodecContext** encCtx);
    void processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs, int64_t trimMs,
                            int64_t jitterMs);
};

#endif // STREAMWORKER_H
