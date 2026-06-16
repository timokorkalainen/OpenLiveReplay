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

enum class IngestBackendKind { Ffmpeg, NativeSrt };

struct IngestBackendOptions {
    bool preferNativeSrt = false;
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

Q_DECLARE_METATYPE(SrtStats)

#endif // INGESTSESSION_H
