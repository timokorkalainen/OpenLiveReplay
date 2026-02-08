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

    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames);
    void writePacket(AVPacket* pkt);
    void writeMetadataPacket(int viewTrack, int64_t pts, const QByteArray& jsonData);
    AVStream* getStream(int index);
    void close();

    int subtitleTrackOffset() const { return m_subtitleTrackOffset; }

    QString getVideoPath(QString fileName);
private:
    AVFormatContext* m_outCtx = nullptr;
    // Track the last timestamp for each stream to ensure they always increase
    QMap<int, int64_t>* m_lastDts;
    QMutex m_mutex;
    bool m_initialized = false;
    int m_subtitleTrackOffset = 0;  // Index of first subtitle track
};

#endif // MUXER_H
