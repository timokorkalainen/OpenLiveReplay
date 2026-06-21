#ifndef OUTPUTFRAMECACHE_H
#define OUTPUTFRAMECACHE_H

#include "playback/output/framehandle.h"
#include "playback/output/mediaframe.h"

#include <QVector>
#include <optional>

class OutputFrameCache {
public:
    OutputFrameCache(int feedCount, int placeholderWidth, int placeholderHeight);

    void insertVideoFrame(const FrameHandle& frame);
    std::optional<FrameHandle> videoFrameAt(int feedIndex, qint64 playheadMs) const;
    FrameHandle videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const;

    void insertAudioFrame(const MediaAudioFrame& frame);
    QByteArray audioSpanOrSilence(int feedIndex, qint64 startSample, int sampleFrames) const;

    int feedCount() const { return m_video.size(); }
    // Insert every frame from `other` (video + audio) without removing the
    // current contents. Feed counts must match. Used to merge a staging window
    // into the live cache before trimming old frames (double-buffer).
    void mergeFrom(const OutputFrameCache& other);
    void trimBefore(qint64 minVideoPtsMs, qint64 minAudioStartSample);
    void clear();

private:
    QVector<QVector<FrameHandle>> m_video;
    QVector<QVector<MediaAudioFrame>> m_audio;
    int m_placeholderWidth = 1920;
    int m_placeholderHeight = 1080;
};

#endif // OUTPUTFRAMECACHE_H
