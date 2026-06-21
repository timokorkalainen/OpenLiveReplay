#ifndef NATIVENDIINGESTSESSION_H
#define NATIVENDIINGESTSESSION_H

#include "ingestsession.h"
#include "ndiframeconvert.h"
#include "recorder_engine/timing/sourceclock.h"

#include <QElapsedTimer>
#include <QStringList>
#include <QUrl>

#include <atomic>
#include <memory>

class INdiReceiverBackend {
public:
    enum class Capture { None, Video, Audio, Error };

    virtual ~INdiReceiverBackend() = default;
    virtual bool isRuntimeAvailable() const = 0;
    virtual bool openReceiver(const QString& sourceName) = 0;
    virtual void closeReceiver() = 0;
    virtual Capture capture(NdiVideoFrame* video, NdiAudioFrame* audio, int timeoutMs) = 0;
    virtual void freeVideo(NdiVideoFrame* video) = 0;
    virtual void freeAudio(NdiAudioFrame* audio) = 0;
};

class NativeNdiIngestSession final : public IngestSession {
public:
    NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning);
    NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning, AnchoredSourceClock* sourceClock);
    NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning, INdiReceiverBackend* backend);
    NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning, INdiReceiverBackend* backend,
                           AnchoredSourceClock* sourceClock);
    ~NativeNdiIngestSession() override;

    static bool supportsUrl(const QUrl& url);
    static bool runtimeAvailable();
    static QString sourceNameFromUrl(const QUrl& url);
    static bool recvCreateSourceIsValueForTest();
    static QStringList runtimeLibraryCandidatesForTest();
    static QString selectDiscoveredSourceForTest(const QStringList& discovered,
                                                 const QString& wanted);

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;
    IngestFailureKind lastFailureKind() const override { return m_lastFailureKind; }

    // Test seam: shorten the stall window so a unit test can exercise the
    // session-internal stall break without waiting the full production timeout.
    void setStallTimeoutMsForTest(int ms) { m_stallTimeoutMs = ms; }

private:
    // Session-internal stall window: if no video/audio frame arrives for this
    // long, run() breaks so captureLoop reconnects — mirrors the SRT/RTMP
    // sessions (their own file-local kStallTimeoutMs and StreamWorker's
    // m_stallTimeoutMs are the same 8000 value; these could converge later).
    static constexpr int kStallTimeoutMs = 8000;
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;
    std::atomic<bool>* m_captureRunning = nullptr;
    std::atomic<bool> m_stopRequested{false};
    IngestCallbacks m_callbacks;
    std::unique_ptr<INdiReceiverBackend> m_ownedBackend;
    INdiReceiverBackend* m_backend = nullptr;
    AnchoredSourceClock m_ownedClock{ClockQuality::Ndi, 10000};
    AnchoredSourceClock* m_clock = &m_ownedClock;
    bool m_externalClock = false;
    QElapsedTimer m_monotonic;
    // Set-before-run (production default, or setStallTimeoutMsForTest before open());
    // read only on the capture thread in run(). Not synchronized — do not mutate
    // once run() has started.
    int m_stallTimeoutMs = kStallTimeoutMs;
    int64_t m_lastFrameAtMs = -1; // m_monotonic.elapsed() at the last received frame
    int64_t m_lastStatsAtMs = -1;
    struct SwsContext* m_sws = nullptr;
    IngestFailureKind m_lastFailureKind = IngestFailureKind::None;

    bool shouldStop() const;
    int64_t mapTimestampMs(int64_t timestamp100ns, ClockObservationRole role);
    void maybeReportStats();
};

#endif // NATIVENDIINGESTSESSION_H
