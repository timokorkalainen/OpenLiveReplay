#ifndef FFMPEGINGESTSESSION_H
#define FFMPEGINGESTSESSION_H

#include "ingestsession.h"

#include <QElapsedTimer>

#include <atomic>
#include <cstdint>

struct AVCodecContext;
struct AVFormatContext;
struct SwrContext;
struct SwsContext;

class FfmpegIngestSession final : public IngestSession {
public:
    explicit FfmpegIngestSession(int sourceIndex);
    FfmpegIngestSession(int sourceIndex, int targetWidth, int targetHeight, int targetFps);
    ~FfmpegIngestSession() override;

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;

    bool isOpen() const;

private:
    static int ffmpegInterruptCallback(void* opaque);

    bool setupDecoder(const QUrl& url);
    void closeAsync();
    bool shouldStop() const;
    void log(const QString& message) const;

    int m_sourceIndex = 0;
    int m_targetWidth = 1920;
    int m_targetHeight = 1080;
    int m_targetFps = 30;

    IngestCallbacks m_callbacks;
    QElapsedTimer m_monotonic;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_readingConnectedStream{false};
    std::atomic<int64_t> m_lastPacketAtMs{-1};
    int m_stallTimeoutMs = 8000;

    AVFormatContext* m_inCtx = nullptr;
    AVCodecContext* m_decCtx = nullptr;
    AVCodecContext* m_audioDecCtx = nullptr;
    SwrContext* m_swrCtx = nullptr;
    SwsContext* m_swsCtx = nullptr;
    int m_videoStreamIdx = -1;
    int m_audioStreamIdx = -1;
};

#endif // FFMPEGINGESTSESSION_H
