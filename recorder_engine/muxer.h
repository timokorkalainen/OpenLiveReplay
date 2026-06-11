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

    bool init(const QString& filename, int videoTrackCount, int width, int height, int fps, const QStringList& streamNames,
             int audioSampleRate = 48000, int audioChannels = 2);
    void writePacket(AVPacket* pkt);
    void writeMetadataPacket(int viewTrack, int64_t ptsMs, const QByteArray& jsonData);
    AVStream* getStream(int index);
    void close();

    int audioTrackOffset() const { return m_audioTrackOffset; }
    int subtitleTrackOffset() const { return m_subtitleTrackOffset; }

    QString getVideoPath(QString fileName);

    // Directory recordings are written to.  Set BEFORE init()/getVideoPath()
    // from the main thread; empty = default (~/Documents/videos).
    // Deliberately unlocked: init() calls getVideoPath() while holding
    // m_mutex, and the value never changes during a recording session.
    void setOutputDirectory(const QString& dir) { m_outputDir = dir; }
private:
    QString m_outputDir;
    // Path resolved by init() for the current session; getVideoPath()
    // returns it while recording so the reader can never diverge from
    // the file actually being written.
    QString m_activePath;
    AVFormatContext* m_outCtx = nullptr;
    // Track the last timestamp for each stream to ensure they always increase
    QMap<int, int64_t>* m_lastDts;
    QMutex m_mutex;
    bool m_initialized = false;
    int m_audioTrackOffset = 0;     // Index of first audio track
    int m_subtitleTrackOffset = 0;  // Index of first subtitle track
};

#endif // MUXER_H
