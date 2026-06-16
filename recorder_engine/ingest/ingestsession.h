#ifndef INGESTSESSION_H
#define INGESTSESSION_H

#include <QByteArray>
#include <QUrl>
#include <QString>

#include <cstdint>
#include <functional>

extern "C" {
struct AVFrame;
}

enum class IngestBackendKind {
    Ffmpeg,
    NativeSrt
};

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

struct IngestCallbacks {
    std::function<bool()> shouldStop;
    std::function<int64_t()> recordingClockMs;
    std::function<void(bool)> setConnected;
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

#endif // INGESTSESSION_H
