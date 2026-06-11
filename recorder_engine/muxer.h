#ifndef MUXER_H
#define MUXER_H

#include <QHash>
#include <QElapsedTimer>
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

    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
             int audioSampleRate = 48000, int audioChannels = 2);
    void writePacket(AVPacket* pkt);
    void writeMetadataPacket(int viewTrack, int64_t ptsMs, const QByteArray& jsonData);
    AVStream* getStream(int index);
    void close();

    int audioTrackOffset() const { return m_audioTrackOffset; }
    int subtitleTrackOffset() const { return m_subtitleTrackOffset; }

    QString getVideoPath(QString fileName);
private:
    AVFormatContext* m_outCtx = nullptr;
    // Last DTS per stream (monotonicity enforcement); guarded by m_mutex
    QHash<int, int64_t> m_lastDts;
    // Throttles avio_flush: flushing per packet hammers the disk for no
    // benefit beyond chase-play visibility (~100 ms is plenty)
    QElapsedTimer m_lastFlush;
    QMutex m_mutex;
    bool m_initialized = false;
    int m_audioTrackOffset = 0;     // Index of first audio track
    int m_subtitleTrackOffset = 0;  // Index of first subtitle track
};

#endif // MUXER_H
