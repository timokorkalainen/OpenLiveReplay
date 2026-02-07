#ifndef MUXER_H
#define MUXER_H

#include <QMutex>
#include <QString>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/opt.h>
}

class Muxer {
public:
    Muxer();
    ~Muxer();

    bool init(const QString& filename, int videoTrackCount);
    void writePacket(AVPacket* pkt);
    AVStream* getStream(int index);
    void close();

    QString getVideoPath(QString fileName);
private:
    AVFormatContext* m_outCtx = nullptr;
    // Track the last timestamp for each stream to ensure they always increase
    QMap<int, int64_t>* m_lastDts;
    QMutex m_mutex;
    bool m_initialized = false;
};

#endif // MUXER_H
