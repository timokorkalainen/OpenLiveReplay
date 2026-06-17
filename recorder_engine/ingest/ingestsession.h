#ifndef INGESTSESSION_H
#define INGESTSESSION_H

#include <QByteArray>
#include <QUrl>
#include <QString>
#include <QMetaType>

#include <cstdint>
#include <functional>

extern "C" {
struct AVFrame;
}

enum class IngestBackendKind { NativeSrt, NativeRtmp, Unsupported };

enum class IngestFailureKind {
    None,
    TransientNetwork,
    UnsupportedProfile,
    DecodeCapability,
    MalformedStream
};

struct IngestOpenResult {
    bool ok = false;
    IngestFailureKind failure = IngestFailureKind::None;
    QString message;
};

struct IngestBackendOptions {
    bool preferNativeSrt = false;
    bool preferNativeRtmp = false;
};

struct DecodedVideoFrame {
    AVFrame* frame = nullptr;
    int64_t sourcePtsMs = 0;
};

struct DecodedAudioChunk {
    int64_t startSample = -1;
    QByteArray pcmS16Stereo;
};

constexpr int kDecodedAudioBytesPerSample = 2 * int(sizeof(int16_t));

// Which backend produced this snapshot — selects the health grader. SRT fills the
// loss-domain counters; RTMP (TCP, no loss) fills the liveness/throughput fields.
enum class IngestStatsKind { Unknown = 0, Srt, Rtmp };

// Per-source ingest stats sampled ~1/sec and pushed to the UI via
// IngestCallbacks::reportStats. Tagged by kind; each backend fills its own fields
// (the others stay 0). See srtHealth()/rtmpHealth() below.
struct IngestStats {
    IngestStatsKind kind = IngestStatsKind::Unknown;
    // SRT (libsrt bstats), cumulative since connect — kind == Srt.
    qint64 recvTotal = 0;    // pktRecvTotal
    qint64 retransTotal = 0; // pktRcvRetrans
    qint64 lossTotal = 0;    // pktRcvLossTotal (detected; retransmitted)
    qint64 dropTotal = 0;    // pktRcvDropTotal (too-late; finally unrecovered)
    // Generic liveness/throughput — kind == Rtmp.
    quint64 bytesTotal = 0;     // cumulative bytes received
    qint64 lastPacketAgeMs = 0; // ms since the last media packet, at sample time
    qint64 keyframeAgeMs = 0;   // ms since the last video keyframe, at sample time
    quint64 decodeFailures = 0; // cumulative frames the native decoder rejected
};

// Per-source link health -> the connection dot: Green=healthy, Amber=stressed, Red=losing content.
enum class SourceHealth { NA = 0, Green = 1, Amber = 2, Red = 3 };

// SRT grader (unchanged logic): Red on any unrecovered drop this window; else Amber
// if the retransmit rate exceeds amberRetransRate; else Green. Negative deltas (a
// reconnect reset the counters) clamp Green. Pure.
SourceHealth srtHealth(const IngestStats& prev, const IngestStats& cur, double amberRetransRate);

// RTMP grader (TCP has no loss): Red if stalled (>= kRtmpRedStallMs since last media)
// or decode is failing with no fresh bytes; Amber on a decode failure this window, a
// brief stall (>= kRtmpAmberStallMs), or a long keyframe gap (>= kRtmpAmberKeyframeMs);
// else Green. A counter reset (cur < prev) clamps Green. Pure.
constexpr int kRtmpRedStallMs = 3000;
constexpr int kRtmpAmberStallMs = 1000;
constexpr int kRtmpAmberKeyframeMs = 5000;
SourceHealth rtmpHealth(const IngestStats& prev, const IngestStats& cur);

// SRT receive latency / connect timeout (milliseconds): the native ingest path
// passes them straight to srt_setsockopt (SRTO_LATENCY / SRTO_CONNTIMEO, which are
// milliseconds).
constexpr int kSrtLatencyMs = 500;
constexpr int kSrtConnectTimeoutMs = 5000;

struct IngestCallbacks {
    std::function<bool()> shouldStop;
    std::function<int64_t()> recordingClockMs;
    std::function<void(bool)> setConnected;
    std::function<void(const IngestStats&)> reportStats;
    std::function<void(const QString&)> logInfo;
    std::function<void(DecodedVideoFrame)> onVideoFrame;
    std::function<void(DecodedAudioChunk)> onAudioChunk;
};

class IngestSession {
public:
    virtual ~IngestSession() = default;

    virtual bool open(const QUrl& url, const IngestCallbacks& callbacks) = 0;
    virtual void run() = 0;
    virtual void requestStop() = 0;
    virtual IngestFailureKind lastFailureKind() const { return IngestFailureKind::None; }
};

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options);
IngestBackendOptions ingestBackendOptionsFromEnvironment(const QUrl& url, bool nativeSrtAvailable,
                                                         bool nativeRtmpAvailable);
bool shouldStopNativeRtmpAfterFailure(IngestFailureKind failure);

Q_DECLARE_METATYPE(IngestStats)

// Per-transport engine jitter window: SRT sources lean on SRT's TSBPD reorder
// buffer, so they need only a small residual floor; other transports get the
// default. Returns srtFloorMs for scheme "srt" (case-insensitive), else defaultMs.
int jitterWindowMs(const QString& scheme, int srtFloorMs, int defaultMs);

#endif // INGESTSESSION_H
