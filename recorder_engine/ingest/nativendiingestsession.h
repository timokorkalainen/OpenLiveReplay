#ifndef NATIVENDIINGESTSESSION_H
#define NATIVENDIINGESTSESSION_H

#include "ingestsession.h"
#include "ndiframeconvert.h"
#include "recorder_engine/timing/sourceclock.h"

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
                           std::atomic<bool>* captureRunning, INdiReceiverBackend* backend);
    ~NativeNdiIngestSession() override;

    static bool supportsUrl(const QUrl& url);
    static bool runtimeAvailable();
    static QString sourceNameFromUrl(const QUrl& url);

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;
    IngestFailureKind lastFailureKind() const override { return m_lastFailureKind; }

private:
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;
    std::atomic<bool>* m_captureRunning = nullptr;
    std::atomic<bool> m_stopRequested{false};
    IngestCallbacks m_callbacks;
    std::unique_ptr<INdiReceiverBackend> m_ownedBackend;
    INdiReceiverBackend* m_backend = nullptr;
    AnchoredSourceClock m_clock{ClockQuality::Ndi, 10000};
    struct SwsContext* m_sws = nullptr;
    IngestFailureKind m_lastFailureKind = IngestFailureKind::None;

    bool shouldStop() const;
    int64_t mapTimestampMs(int64_t timestamp100ns, ClockObservationRole role);
};

#endif // NATIVENDIINGESTSESSION_H
