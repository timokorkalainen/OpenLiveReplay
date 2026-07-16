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
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <thread>

#include "recordingclock.h"
#include "muxer.h"
#include "ingest/decodedframeevidencequeue.h"
#include "ingest/ingestsession.h"
#include "timing/sourceclock.h"
#include "timing/timecodeevidence.h"

#include "recorder_engine/codec/videocodecchoice.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#if defined(OLR_GPU_PIPELINE_BUILD)
#include "playback/output/framehandle.h"
#include "recorder_engine/codec/gpuencodepump.h"
#endif

class GpuEncodePump;
class GpuFence;
#if defined(_WIN32)
class WinGpuImportEdge;
#endif

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
    // targetFps is the rounded integer rate (drives the internal ms cadence);
    // targetFpsNum/targetFpsDen are the rational rate the encoder advertises
    // (e.g. 30000/1001). 0/0 falls back to {targetFps, 1}.
    StreamWorker(const QString& url, int sourceIndex, Muxer* muxer, RecordingClock* clock,
                 int targetWidth, int targetHeight, int targetFps, int targetFpsNum,
                 int targetFpsDen, VideoCodecChoice codec = VideoCodecChoice::Mpeg2Software,
                 QObject* parent = nullptr);
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
    uint64_t currentCarrierEpoch() const { return m_carrierEpoch.load(std::memory_order_acquire); }

#ifdef OLR_UNIT_TEST
    friend class TestStreamWorkerGpuEncode;
    friend class TestReplayManagerTimecode;
    const GpuEncodePump* gpuEncodePumpForTest() const;
    bool preferGpuVideoFramesForIngestForTest() const;
    bool ensureGpuEncodePumpStartedForTest();
#endif

signals:
    // Drained on this QObject's worker thread only when the capture-side state
    // flips. ReplayManager receives it through a queued connection.
    void connectionChanged(int sourceIndex, bool connected);

    // Emitted ~1/sec from the capture thread with the source's latest ingest stats
    // (native SRT or RTMP; tagged by IngestStats::kind). Cross-thread: relayed to the
    // UI through ReplayManager with a queued connection, like connectionChanged.
    void statsUpdated(int sourceIndex, IngestStats stats);

    // Emitted after the muxer confirms that the selected frame's packet was written.
    // The evidence is rebound to the session frame where it was muxed and consumed
    // one-shot, so held CFR frames cannot report the same observation twice.
    void frameTimecode(int sourceIndex, uint64_t carrierEpoch, TimecodeEvidence evidence);

public slots:
    void onMasterPulse(int64_t frameIndex, int64_t streamTimeMs);

protected:
    void run() override;

private:
    struct SourceCarrierToken {
        uint64_t sessionIdentity = 0;
        uint64_t epoch = 0;
    };
    struct MuxFrameEvidenceSubmission {
        uint64_t id = 0;
        SourceCarrierToken carrierToken;
    };

    QString m_url;
    int m_sourceIndex;            // Fixed: identity of this source
    std::atomic<int> m_viewTrack; // Dynamic: muxer track to write to (-1 = none)
    Muxer* m_muxer;

    AVFrame* m_latestFrame = nullptr;
    // Source timecode (100 ns since midnight) of the frame currently held in
    // m_latestFrame, or -1 when none/blue. Retained for the recording start tag;
    // alignment uses m_latestFrameTimecodeEvidence below.
    std::atomic<int64_t> m_latestFrameTimecode100ns{-1};
    std::optional<TimecodeEvidence> m_latestFrameTimecodeEvidence;
    std::shared_ptr<const SourceCarrierToken> m_latestFrameCarrierToken;
    int64_t m_internalFrameCount;
    RecordingClock* m_sharedClock;

    QAtomicInt m_restartCapture; // Thread-safe flag to signal a source swap
    QAtomicInt m_paintBlue{0};   // Deferred blue-paint flag

    // Set when the source is changed to an empty URL (blue-paint state),
    // cleared when a non-empty URL actually connects.  While set, the
    // capture thread refuses to enqueue frames so a late straggler decoded
    // from the old/cleared source cannot overwrite the painted blue frame.
    std::atomic<bool> m_suppressEnqueue{false};

    // Mutexes & Threads
    QMutex m_frameMutex;
    QMutex m_urlMutex;
    QMutex m_metadataMutex;
    QMutex m_sessionMutex;
    mutable std::mutex m_epochMutex;
    uint64_t m_nextCaptureSessionIdentity = 0;
    uint64_t m_activeCaptureSessionIdentity = 0;
    std::shared_ptr<const SourceCarrierToken> m_activeCarrierToken;
    std::atomic<uint64_t> m_carrierEpoch{1};
    QByteArray m_sourceMetadataJson;          // JSON blob for per-frame subtitle track
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
    std::mutex m_connectionTransitionMutex;
    std::deque<bool> m_pendingConnectionEmissions;
    bool m_connectionDrainScheduled = false;
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
    void setConnectedImpl(bool connected, uint64_t requiredSessionIdentity);
    void drainConnectionEmissions();

    // Last jitter-pull gate published by the tick thread (file-timeline ms,
    // -1 until the first tick).  The capture thread uses it to pre-drain
    // frames the next tick would discard anyway.
    std::atomic<int64_t> m_lastTickTargetMs{-1};

    // Audio FIFO: the capture thread produces resampled 48 kHz stereo S16
    // stamped on the global recording timeline; the master-pulse tick
    // consumes it on a sample-accurate cursor (gap-filled with silence).
    QMutex m_audioFifoMutex;
    QByteArray m_audioFifo;
    int64_t m_audioFifoStartSample = -1; // timeline sample index of m_audioFifo[0]
    int64_t m_audioWriteCursor = -1;     // next sample to mux (tick thread only)
    int64_t m_audioSourceCursor = -1;    // next source-timeline sample to consume
    int64_t m_audioServoTrimSamples = 0;
    int64_t m_audioServoJitterSamples = 0;
    void enqueueAudio(int64_t startSample, const uint8_t* data, int numSamples);
    void enqueueAudioForSession(uint64_t sessionIdentity, int64_t startSample, const uint8_t* data,
                                int numSamples);
    void setConnectedForSession(uint64_t sessionIdentity, bool connected);
    void reportStatsForSession(uint64_t sessionIdentity, const IngestStats& stats);
    void writeAudioForTick(int64_t recordingTimeMs, int track, int64_t trimMs, int64_t jitterMs);

    int m_targetWidth = 1920;
    int m_targetHeight = 1080;
    int m_targetFps = 30;    // rounded integer rate (internal ms cadence)
    int m_targetFpsNum = 30; // advertised rational rate numerator (e.g. 30000)
    int m_targetFpsDen = 1;  // advertised rational rate denominator (e.g. 1001)
    VideoCodecChoice m_videoCodec = VideoCodecChoice::Mpeg2Software;

    // FFmpeg context management
    struct QueuedFrame {
        AVFrame* frame{};
        int64_t sourcePts{};
        // The transport's 100 ns label is retained only for the recording start tag.
        // Alignment consumes the full typed evidence value alongside the frame.
        int64_t sourceTimecode100ns = -1;
        std::optional<TimecodeEvidence> timecodeEvidence;
        std::shared_ptr<const SourceCarrierToken> carrierToken;
#ifdef OLR_GPU_PIPELINE_BUILD
        FrameHandle gpuFrame;
        uint64_t gpuFenceValue = 0;
#endif
    };

    static qint64 queuedFrameBytes(const QueuedFrame& frame);
    qint64 frameQueueBackstopBytes() const;
    void trimFrameQueueBackstopLocked(qint64 tickGateMs);

    QQueue<QueuedFrame> m_frameQueue;
    AVCodecContext* m_persistentEncCtx = nullptr;
    std::unique_ptr<NativeVideoEncoder> m_nativeEncoder;
    std::mutex m_nativeEncodeMutex;

#ifdef OLR_GPU_PIPELINE_BUILD
    std::unique_ptr<GpuEncodePump> m_gpuEncodePump;
    std::shared_ptr<GpuFence> m_gpuEncodeFence;
    std::atomic<bool> m_gpuEncodeCpuFallback{false};
#if defined(_WIN32)
    std::unique_ptr<WinGpuImportEdge> m_gpuEncodeImportEdge;
#endif
    FrameHandle m_latestGpuFrame;
    uint64_t m_latestGpuFenceValue = 0;
    std::atomic<int64_t> m_latestGpuFrameTimecode100ns{-1};
    std::shared_ptr<const TimecodeEvidence> m_latestGpuFrameTimecodeEvidence;
    std::shared_ptr<const SourceCarrierToken> m_latestGpuFrameCarrierToken;
#endif

    // FFmpeg helpers
    bool setupEncoder(AVCodecContext** encCtx);
#ifdef OLR_GPU_PIPELINE_BUILD
    ImportedGpuVideoFrame importGpuVideoFrameForEncode(void* nativeDecodedImage,
                                                       const FrameMetadata& metadata,
                                                       bool latchFallbackOnFailure = true);
    ImportedGpuVideoFrame importGpuVideoFrameForSession(uint64_t sessionIdentity,
                                                        void* nativeDecodedImage,
                                                        const FrameMetadata& metadata);
    bool ensureGpuEncodePumpStarted();
    bool preferGpuVideoFramesForIngest() const;
    bool tryLatchGpuEncodeCpuFallback(const SourceCarrierToken& failureCarrier);
    void latchGpuEncodeCpuFallback();
#endif
    static constexpr size_t kSubmissionPoolCapacity = Muxer::kMaxQueuedPackets + 2;
    static constexpr size_t kMuxCompletionPoolCapacity = Muxer::kMaxQueuedPackets + 2;
    static constexpr size_t kMaxPacketsPerSubmission = 8;
    struct BufferedEncodedPacket {
        QByteArray data;
        int64_t ptsTicks = 0;
        bool keyframe = false;
    };
    struct EncodeSubmissionSlot {
        bool active = false;
        uint32_t generation = 0;
        bool gpu = false;
        bool encoderFinished = false;
        bool evidenceResolved = false;
        bool sidecarsEmitted = false;
        bool fallbackTriggered = false;
        uint32_t pendingWrites = 0;
        int track = -1;
        AVStream* stream = nullptr;
        bool* havePacket = nullptr;
        SourceCarrierToken carrierToken;
        uint64_t evidenceSubmissionId = 0;
        int64_t streamTimeMs = 0;
        QByteArray metadata;
        std::array<BufferedEncodedPacket, kMaxPacketsPerSubmission> bufferedPackets;
        size_t bufferedPacketCount = 0;
        size_t nextBufferedPacket = 0;
        bool packetOverflow = false;
    };
    struct MuxCompletionSlot {
        bool active = false;
        uint32_t generation = 0;
        SourceCarrierToken carrierToken;
        uint64_t encodeSubmissionId = 0;
        std::optional<TimecodeEvidence> evidence;
    };
    uint64_t acquireEncodeSubmission(bool gpu, int track, AVStream* stream, bool* havePacket,
                                     const SourceCarrierToken& carrierToken,
                                     uint64_t evidenceSubmissionId, int64_t streamTimeMs = 0,
                                     const QByteArray& metadata = QByteArray());
    bool setEncodeSubmissionEvidenceId(uint64_t submissionId, uint64_t evidenceSubmissionId);
    uint64_t reserveMuxCompletion(uint64_t encodeSubmissionId);
    uint64_t reserveMuxCompletion(uint64_t encodeSubmissionId,
                                  const SourceCarrierToken& carrierToken);
    void releaseMuxCompletionReservation(uint64_t completionId);
    bool populateMuxCompletion(uint64_t completionId, const SourceCarrierToken& carrierToken,
                               std::optional<TimecodeEvidence> evidence);
    void finishEncodeSubmission(uint64_t submissionId);
    void failEncodeSubmission(uint64_t submissionId);
    void handleEncodedPacket(uint64_t submissionId, const QByteArray& data, int64_t ptsTicks,
                             bool keyframe);
    void bufferEncodedPacket(uint64_t submissionId, const QByteArray& data, int64_t ptsTicks,
                             bool keyframe);
    bool bufferedSubmissionReady(uint64_t submissionId) const;
    bool takeBufferedSubmissionPacket(uint64_t submissionId, BufferedEncodedPacket* packet);
    void commitBufferedEncodeSubmission(uint64_t submissionId);
    void completeMuxWrite(uint64_t completionId, bool written);
    static void encodedPacketThunk(void* context, uint64_t submissionId, const QByteArray& data,
                                   int64_t ptsTicks, bool keyframe);
    static void bufferedPacketThunk(void* context, uint64_t submissionId, const QByteArray& data,
                                    int64_t ptsTicks, bool keyframe);
    static void encodeFailureThunk(void* context, uint64_t submissionId);
    static void encodeFinishedThunk(void* context, uint64_t submissionId);
    static void bufferedEncodeFinishedThunk(void* context, uint64_t submissionId);
    static void muxWriteCompletionThunk(void* context, uint64_t completionId, bool written);
    static bool packetCarrierGuardThunk(void* context, uint64_t sessionIdentity, uint64_t epoch);
    NativeVideoEncoder::PacketCallback packetCallbackForSubmission(uint64_t submissionId) noexcept;
    NativeVideoEncoder::PacketCallback
    bufferedPacketCallbackForSubmission(uint64_t submissionId) noexcept;
#if defined(OLR_GPU_PIPELINE_BUILD)
    GpuEncodePump::JobCallbacks gpuCallbacksForSubmission(uint64_t submissionId) noexcept;
#endif
    Muxer::PacketWriteCallback muxCompletionCallback(uint64_t completionId) noexcept;
    static uint64_t poolId(size_t index, uint32_t generation) noexcept;
    static bool decodePoolId(uint64_t id, size_t capacity, size_t* index,
                             uint32_t* generation) noexcept;
    void releaseEncodeSubmissionLocked(size_t index);
    mutable std::mutex m_submissionPoolMutex;
    std::unique_ptr<EncodeSubmissionSlot[]> m_submissionPool;
    std::unique_ptr<MuxCompletionSlot[]> m_muxCompletionPool;
    size_t m_nextSubmissionSlot = 0;
    size_t m_nextMuxCompletionSlot = 0;
    void enqueueDecodedVideoFrame(DecodedVideoFrame decoded);
    void enqueueDecodedVideoFrameForSession(DecodedVideoFrame decoded, uint64_t sessionIdentity);
    uint64_t beginCaptureSession();
    void endCaptureSession(uint64_t sessionIdentity);
    std::shared_ptr<const SourceCarrierToken> snapshotActiveCarrierToken() const;
    std::shared_ptr<const SourceCarrierToken>
    snapshotCarrierTokenForSession(uint64_t sessionIdentity) const;
    std::shared_ptr<const SourceCarrierToken>
    prepareCarrierTokenForFrameIngress(uint64_t sessionIdentity,
                                       const std::optional<TimecodeEvidence>& evidence);
    std::shared_ptr<const SourceCarrierToken>
    prepareCarrierTokenForFrameIngressLocked(uint64_t sessionIdentity,
                                             const std::optional<TimecodeEvidence>& evidence);
    void rotateCarrier(bool retainActiveSession);
    void rotateCarrierLocked(bool retainActiveSession);
    bool carrierTokenIsCurrent(const std::shared_ptr<const SourceCarrierToken>& token) const;
    bool carrierTokenIsCurrent(const SourceCarrierToken& token) const;
    bool carrierTokenIsCurrentLocked(const SourceCarrierToken& token) const;
    Muxer::PacketCarrierGuard packetCarrierGuard(const SourceCarrierToken& token) noexcept;
    std::optional<TimecodeEvidence>
    takeFrameTimecodeEvidenceForMux(std::optional<TimecodeEvidence>& selected,
                                    int64_t sessionFrameIndex) const;
    MuxFrameEvidenceSubmission
    enqueueMuxFrameEvidence(int64_t ptsTicks, int64_t sourceTimecode100ns,
                            const std::optional<TimecodeEvidence>& evidence,
                            const std::shared_ptr<const SourceCarrierToken>& frameToken,
                            bool allowSyntheticFrame = false);
    std::optional<DecodedFrameEvidence> takeMuxFrameEvidence(int64_t ptsTicks);
    std::optional<DecodedFrameEvidence>
    takeMuxFrameEvidence(int64_t ptsTicks, const SourceCarrierToken& expectedToken);
    void discardMuxFrameEvidence(uint64_t submissionId);
    void clearMuxFrameEvidence();
    void resetMuxFrameEvidenceLocked();
    bool muxFrameEvidenceIsCurrent(uint64_t epoch) const;
    void emitFrameTimecodeEvidence(const TimecodeEvidence& evidence, uint64_t carrierEpoch);
#ifdef OLR_UNIT_TEST
    // One-shot seam for exercising the real write-rejection branch after frame
    // selection and encoding. Success-path tests leave it empty and use the real
    // asynchronous Muxer writer/completion unchanged.
    void runBeforeMuxPacketWriteForTest() {
        auto hook = std::move(m_beforeMuxPacketWriteForTest);
        if (hook) hook();
    }
    void runBeforeMuxEvidenceSubmissionForTest() {
        auto hook = std::move(m_beforeMuxEvidenceSubmissionForTest);
        if (hook) hook();
    }
    void runBeforeGpuFallbackTryForTest() {
        auto hook = std::move(m_beforeGpuFallbackTryForTest);
        if (hook) hook();
    }
    std::function<void()> m_beforeMuxPacketWriteForTest;
    std::function<void()> m_beforeMuxEvidenceSubmissionForTest;
    std::function<void()> m_beforeGpuFallbackTryForTest;
#endif
    mutable std::mutex m_muxFrameEvidenceMutex;
    DecodedFrameEvidenceQueue m_muxFrameEvidence{64};
    std::optional<TimecodeEvidence> m_muxFrameEvidenceIdentity;
    void processEncoderTick(AVCodecContext* encCtx, int64_t streamTimeMs, int64_t trimMs,
                            int64_t jitterMs);
};

#endif // STREAMWORKER_H
