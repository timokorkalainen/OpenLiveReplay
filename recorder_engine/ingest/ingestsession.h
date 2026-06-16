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

enum class IngestBackendKind { Ffmpeg, NativeSrt, NativeRtmp };

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

// Cumulative SRT receiver counters sampled from srt_bstats (native ingest only).
// Snapshots are pushed to the UI ~1/sec via IngestCallbacks::reportStats so the
// connection-status dot can grade link health. See srtHealth() below.
struct SrtStats {
    qint64 recvTotal = 0;    // pktRecvTotal
    qint64 retransTotal = 0; // pktRcvRetrans   (received retransmissions; healthy when small)
    qint64 lossTotal = 0;    // pktRcvLossTotal (DETECTED loss; retransmitted)
    qint64 dropTotal = 0;    // pktRcvDropTotal (too-late-to-play; finally UNRECOVERED)
};

// Per-source link health derived from the delta between two SrtStats snapshots.
// Maps to the connection dot: Green=healthy, Amber=link stressed, Red=losing content.
enum class SrtHealth { NA = 0, Green = 1, Amber = 2, Red = 3 };

// Classify the most-recent sampling window (cur - prev). Red if any unrecovered
// drops occurred; else Amber if the retransmit rate exceeds amberRetransRate
// (fraction of received packets, e.g. 0.02); else Green. Negative deltas (a
// reconnect reset the socket counters) clamp to Green. Pure — no Qt/UI deps.
SrtHealth srtHealth(const SrtStats& prev, const SrtStats& cur, double amberRetransRate);

// Shared SRT receive latency / connect timeout (milliseconds), used by BOTH ingest
// paths: the native path passes them straight to srt_setsockopt (SRTO_LATENCY /
// SRTO_CONNTIMEO, which are milliseconds); the ffmpeg path puts them in the URL query
// via augmentSrtUrl() (note: ffmpeg's latency options are MICROSECONDS — see there).
constexpr int kSrtLatencyMs = 500;
constexpr int kSrtConnectTimeoutMs = 5000;

struct IngestCallbacks {
    std::function<bool()> shouldStop;
    std::function<int64_t()> recordingClockMs;
    std::function<void(bool)> setConnected;
    std::function<void(const SrtStats&)> reportStats;
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
    virtual QString nativeFallbackReason() const { return QString(); }
};

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options);
IngestBackendOptions ingestBackendOptionsFromEnvironment(const QUrl& url, bool nativeSrtAvailable,
                                                         bool nativeRtmpAvailable);

Q_DECLARE_METATYPE(SrtStats)

// Append SRT-private options to an srt:// URL's query so they actually apply.
// FFmpeg's libsrt reads these via the URL query (av_find_info_tag); set on the
// avformat_open_input() opts dict they are silently ignored. Non-srt URLs are
// returned unchanged; an option already present in the query is left as-is (a
// user override wins). UNITS: ffmpeg's latency/rcvlatency/peerlatency are
// MICROSECONDS (it divides by 1000 -> SRTO_LATENCY ms), so they carry
// kSrtLatencyMs*1000; connect_timeout is milliseconds; linger is seconds.
QUrl augmentSrtUrl(const QUrl& url);

// Per-transport engine jitter window: SRT sources lean on SRT's TSBPD reorder
// buffer, so they need only a small residual floor; other transports get the
// default. Returns srtFloorMs for scheme "srt" (case-insensitive), else defaultMs.
int jitterWindowMs(const QString& scheme, int srtFloorMs, int defaultMs);

#endif // INGESTSESSION_H
