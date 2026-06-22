#include "playback/output/outputframecache.h"

#include <algorithm>
#include <cstring>

OutputFrameCache::OutputFrameCache(int feedCount, int placeholderWidth, int placeholderHeight)
    : m_video(qMax(0, feedCount)), m_audio(qMax(0, feedCount)),
      m_placeholderWidth(qMax(2, placeholderWidth)),
      m_placeholderHeight(qMax(2, placeholderHeight)) {}

void OutputFrameCache::insertVideoFrame(const FrameHandle& frame,
                                        EvictedVideoFrames* evictedFrames) {
    const FramePayloadKey& key = frame.metadata().key;
    if (key.feedIndex < 0 || key.feedIndex >= m_video.size() || !frame.isPresentable()) return;
    auto& list = m_video[key.feedIndex];
    auto it =
        std::lower_bound(list.begin(), list.end(), key.ptsMs, [](const FrameHandle& f, qint64 pts) {
            return f.metadata().key.ptsMs < pts;
        });
    if (it != list.end() && it->metadata().key.ptsMs == key.ptsMs) {
        if (evictedFrames) evictedFrames->append(*it);
        *it = frame;
    } else {
        list.insert(it, frame);
    }
}

std::optional<FrameHandle> OutputFrameCache::videoFrameAt(int feedIndex, qint64 playheadMs) const {
    if (feedIndex < 0 || feedIndex >= m_video.size()) return std::nullopt;
    const auto& list = m_video[feedIndex];
    if (list.isEmpty()) return std::nullopt;
    auto it = std::upper_bound(
        list.begin(), list.end(), playheadMs,
        [](qint64 pts, const FrameHandle& f) { return pts < f.metadata().key.ptsMs; });
    if (it == list.begin()) return std::nullopt;
    --it;
    return *it;
}

FrameHandle OutputFrameCache::videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const {
    auto frame = videoFrameAt(feedIndex, playheadMs);
    if (frame.has_value()) return *frame;
    FrameHandle placeholder =
        solidYuv420pHandle(m_placeholderWidth, m_placeholderHeight, 16, 128, 128);
    placeholder.metadata().key.feedIndex = feedIndex;
    placeholder.metadata().key.ptsMs = playheadMs;
    placeholder.metadata().key.isPlaceholder = true;
    return placeholder;
}

OutputFrameCache::EvictedVideoFrames OutputFrameCache::videoFramesSnapshot() const {
    EvictedVideoFrames frames;
    for (const auto& feedFrames : m_video)
        frames += feedFrames;
    return frames;
}

void OutputFrameCache::insertAudioFrame(const MediaAudioFrame& frame) {
    if (frame.feedIndex < 0 || frame.feedIndex >= m_audio.size()) return;
    if (frame.format != MediaSampleFormat::S16Interleaved || frame.channels != 2) return;
    auto& list = m_audio[frame.feedIndex];
    auto it = std::lower_bound(
        list.begin(), list.end(), frame.startSample,
        [](const MediaAudioFrame& f, qint64 sample) { return f.startSample < sample; });
    list.insert(it, frame);
}

QByteArray OutputFrameCache::audioSpanOrSilence(int feedIndex, qint64 startSample,
                                                int sampleFrames) const {
    QByteArray out = silentS16Stereo(sampleFrames);
    if (feedIndex < 0 || feedIndex >= m_audio.size() || sampleFrames <= 0) return out;

    const qint64 endSample = startSample + sampleFrames;
    const int bytesPerFrame = 2 * int(sizeof(qint16));
    for (const MediaAudioFrame& frame : m_audio[feedIndex]) {
        const qint64 frameStart = frame.startSample;
        const qint64 frameEnd = frame.startSample + frame.sampleFrames();
        const qint64 copyStart = qMax(startSample, frameStart);
        const qint64 copyEnd = qMin(endSample, frameEnd);
        if (copyEnd <= copyStart) continue;

        const qint64 dstOffset = (copyStart - startSample) * bytesPerFrame;
        const qint64 srcOffset = (copyStart - frameStart) * bytesPerFrame;
        const qint64 bytes = (copyEnd - copyStart) * bytesPerFrame;
        std::memcpy(out.data() + dstOffset, frame.pcm.constData() + srcOffset, size_t(bytes));
    }
    return out;
}

void OutputFrameCache::mergeFrom(const OutputFrameCache& other, EvictedVideoFrames* evictedFrames) {
    const qsizetype feeds = qMin(m_video.size(), other.m_video.size());
    for (qsizetype feed = 0; feed < feeds; ++feed) {
        for (const FrameHandle& frame : other.m_video.at(feed))
            insertVideoFrame(frame, evictedFrames);
    }
    const qsizetype aFeeds = qMin(m_audio.size(), other.m_audio.size());
    for (qsizetype feed = 0; feed < aFeeds; ++feed) {
        for (const MediaAudioFrame& frame : other.m_audio.at(feed))
            insertAudioFrame(frame);
    }
}

void OutputFrameCache::trimBefore(qint64 minVideoPtsMs, qint64 minAudioStartSample,
                                  EvictedVideoFrames* evictedFrames) {
    for (auto& frames : m_video) {
        int firstAtOrAfter = 0;
        while (firstAtOrAfter < frames.size() &&
               frames[firstAtOrAfter].metadata().key.ptsMs < minVideoPtsMs) {
            ++firstAtOrAfter;
        }

        // Keep one frame before the cutoff so output can still hold the nearest
        // previous picture at the retained-window boundary.
        const int removeCount = qMax(0, firstAtOrAfter - 1);
        if (removeCount > 0) {
            if (evictedFrames) {
                for (int i = 0; i < removeCount; ++i)
                    evictedFrames->append(frames[i]);
            }
            frames.erase(frames.begin(), frames.begin() + removeCount);
        }
    }

    for (auto& frames : m_audio) {
        int removeCount = 0;
        while (removeCount < frames.size() &&
               frames[removeCount].startSample + frames[removeCount].sampleFrames() <=
                   minAudioStartSample) {
            ++removeCount;
        }
        if (removeCount > 0) frames.erase(frames.begin(), frames.begin() + removeCount);
    }
}

void OutputFrameCache::clear(EvictedVideoFrames* evictedFrames) {
    for (auto& frames : m_video) {
        if (evictedFrames) *evictedFrames += frames;
        frames.clear();
    }
    for (auto& frames : m_audio)
        frames.clear();
}
