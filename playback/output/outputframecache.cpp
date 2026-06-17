#include "playback/output/outputframecache.h"

#include <algorithm>
#include <cstring>

OutputFrameCache::OutputFrameCache(int feedCount, int placeholderWidth, int placeholderHeight)
    : m_video(qMax(0, feedCount)), m_audio(qMax(0, feedCount)),
      m_placeholderWidth(qMax(2, placeholderWidth)),
      m_placeholderHeight(qMax(2, placeholderHeight)) {}

void OutputFrameCache::insertVideoFrame(const MediaVideoFrame& frame) {
    if (frame.feedIndex < 0 || frame.feedIndex >= m_video.size() || !frame.isValid()) return;
    auto& list = m_video[frame.feedIndex];
    auto it = std::lower_bound(list.begin(), list.end(), frame.ptsMs,
                               [](const MediaVideoFrame& f, qint64 pts) { return f.ptsMs < pts; });
    if (it != list.end() && it->ptsMs == frame.ptsMs) {
        *it = frame;
    } else {
        list.insert(it, frame);
    }
}

std::optional<MediaVideoFrame> OutputFrameCache::videoFrameAt(int feedIndex,
                                                              qint64 playheadMs) const {
    if (feedIndex < 0 || feedIndex >= m_video.size()) return std::nullopt;
    const auto& list = m_video[feedIndex];
    if (list.isEmpty()) return std::nullopt;
    auto it = std::upper_bound(list.begin(), list.end(), playheadMs,
                               [](qint64 pts, const MediaVideoFrame& f) { return pts < f.ptsMs; });
    if (it == list.begin()) return std::nullopt;
    --it;
    return *it;
}

MediaVideoFrame OutputFrameCache::videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const {
    auto frame = videoFrameAt(feedIndex, playheadMs);
    if (frame.has_value()) return *frame;
    MediaVideoFrame placeholder =
        MediaVideoFrame::solidYuv420p(m_placeholderWidth, m_placeholderHeight, 16, 128, 128);
    placeholder.feedIndex = feedIndex;
    placeholder.ptsMs = playheadMs;
    placeholder.isPlaceholder = true;
    return placeholder;
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

void OutputFrameCache::clear() {
    for (auto& frames : m_video)
        frames.clear();
    for (auto& frames : m_audio)
        frames.clear();
}
