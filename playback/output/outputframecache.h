#ifndef OUTPUTFRAMECACHE_H
#define OUTPUTFRAMECACHE_H

#include "playback/output/mediaframe.h"

#include <QVector>
#include <optional>

class OutputFrameCache {
public:
    OutputFrameCache(int feedCount, int placeholderWidth, int placeholderHeight);

    void insertVideoFrame(const MediaVideoFrame& frame);
    std::optional<MediaVideoFrame> videoFrameAt(int feedIndex, qint64 playheadMs) const;
    MediaVideoFrame videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const;

    void insertAudioFrame(const MediaAudioFrame& frame);
    QByteArray audioSpanOrSilence(int feedIndex, qint64 startSample, int sampleFrames) const;

    int feedCount() const { return m_video.size(); }
    void trimBefore(qint64 minVideoPtsMs, qint64 minAudioStartSample);
    void clear();

private:
    QVector<QVector<MediaVideoFrame>> m_video;
    QVector<QVector<MediaAudioFrame>> m_audio;
    int m_placeholderWidth = 1920;
    int m_placeholderHeight = 1080;
};

#endif // OUTPUTFRAMECACHE_H
