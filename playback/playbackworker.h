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
#include "frameprovider.h"
#include "playback/playbacktransport.h"

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
}

struct DecoderTrack {
    AVCodecContext* codecCtx = nullptr;
    FrameProvider* provider = nullptr;
    int streamIndex = -1;
};

class PlaybackWorker : public QThread {
    Q_OBJECT
public:
    explicit PlaybackWorker(const QList<FrameProvider*> &providers, PlaybackTransport *transport, QObject *parent = nullptr);
    ~PlaybackWorker();

    void openFile(const QString &filePath);
    void seekTo(int64_t timestampMs);
    void stop();

protected:
    void run() override;

private:
    // High-performance conversion from FFmpeg AVFrame to QVideoFrame (YUV420P)
    QVideoFrame convertToQVideoFrame(AVFrame* frame);

    QList<FrameProvider*> m_providers;
    QVector<DecoderTrack*> m_decoderBank;
    QHash<int, DecoderTrack*> m_streamMap;
    AVFormatContext* m_fmtCtx = nullptr;

    bool m_running = false;
    int64_t m_seekTargetMs = -1;
    QString m_currentFilePath;
    PlaybackTransport *m_transport;

    QMutex m_mutex;
};

#endif // PLAYBACKWORKER_H
