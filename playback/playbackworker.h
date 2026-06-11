#ifndef PLAYBACKWORKER_H
#define PLAYBACKWORKER_H

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <QThread>
#include <QVector>
#include <QMutex>
#include <QVideoFrame>
#include <QHash>
#include <QList>
#include <atomic>
#include "frameprovider.h"
#include "playback/playbacktransport.h"
#include "playback/audioplayer.h"

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/error.h>
}

struct BufferedFrame {
    int64_t ptsMs = -1;
    QVideoFrame frame;
};

struct DecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    FrameProvider* provider = nullptr;
    int streamIndex = -1;
    QVector<BufferedFrame> buffer;
    int64_t lastDeliveredPtsMs = -1;   // last frame released to the provider
};

struct AudioDecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    int streamIndex = -1;
    int viewIndex   = -1;   // which view (0..N-1) this audio belongs to
};

class PlaybackWorker : public QThread {
    Q_OBJECT
public:
    struct PlaybackCounters {
        int reposition = 0, reuseSeek = 0, reverseChunkSeek = 0,
            eofTailSeek = 0, skipForward = 0, audioPushes = 0, framesDropped = 0;
    };

    explicit PlaybackWorker(const QList<FrameProvider*> &providers, PlaybackTransport *transport,
                            AudioPlayer *audioPlayer = nullptr, QObject *parent = nullptr);
    ~PlaybackWorker();

    void openFile(const QString &filePath);
    void seekTo(int64_t timestampMs);
    bool deliverBufferedFrameAtOrBefore(int64_t targetMs);
    void deliverDueFrames(int64_t masterTimeMs);
    void setFrameBufferMax(int maxFrames);
    void setActiveAudioView(int viewIndex);
    void stop();

    PlaybackCounters counters() const { return m_counters; }

protected:
    void run() override;

private:
    // High-performance conversion from FFmpeg AVFrame to QVideoFrame (YUV420P)
    QVideoFrame convertToQVideoFrame(AVFrame* frame);

    // Push a decoded audio frame, PTS-tagged, to the audio player
    void pushAudioFrame(AudioDecoderTrack* aTrack, AVFrame* audioFrame);

    static int ffmpegInterruptCallback(void* opaque);
    bool shouldInterrupt() const;

    QList<FrameProvider*> m_providers;
    QVector<DecoderTrack*> m_decoderBank;
    QVector<AudioDecoderTrack*> m_audioDecoderBank;
    QHash<int, DecoderTrack*> m_streamMap;
    AVFormatContext* m_fmtCtx = nullptr;

    std::atomic<bool> m_running{false};
    int64_t m_seekTargetMs = -1;
    QString m_currentFilePath;
    PlaybackTransport *m_transport;
    AudioPlayer *m_audioPlayer = nullptr;
    std::atomic<int> m_activeAudioView{-1};

    int m_frameBufferMax = 30;

    QMutex m_mutex;
    QMutex m_bufferMutex;

    PlaybackCounters m_counters;
    void emitTelemetry(int64_t P, int64_t newest, double speed);
};

#endif // PLAYBACKWORKER_H
