#include "playback/output/outputbusengine.h"

#include "playback/output/yuv420pcompositor.h"

OutputBusEngine::OutputBusEngine(FrameRate rate, int feedCount, int width, int height)
    : m_clock(rate), m_feedCount(qMax(0, feedCount)), m_width(qMax(2, width)),
      m_height(qMax(2, height)) {}

void OutputBusEngine::setTargetAssignments(const QList<OutputTargetAssignment>& assignments) {
    m_assignments = assignments;
}

int OutputBusEngine::audioSamplesPerFrame() const {
    return audioSamplesForOutputFrame(0);
}

OutputBusFrame OutputBusEngine::renderFeed(int feedIndex, qint64 outputFrameIndex,
                                           const PlaybackStateSnapshot& state,
                                           const OutputFrameCache& cache) const {
    return renderSingleSource(OutputBusId::feed(feedIndex), feedIndex, outputFrameIndex, state,
                              cache, true);
}

OutputBusFrame OutputBusEngine::renderPgm(qint64 outputFrameIndex,
                                          const PlaybackStateSnapshot& state,
                                          const OutputFrameCache& cache) const {
    return renderSingleSource(OutputBusId::pgm(), state.selectedFeedIndex, outputFrameIndex, state,
                              cache, true);
}

OutputBusFrame OutputBusEngine::renderMultiview(qint64 outputFrameIndex,
                                                const PlaybackStateSnapshot& state,
                                                const OutputFrameCache& cache) const {
    OutputBusFrame out;
    out.bus = OutputBusId::multiview();
    out.outputFrameIndex = outputFrameIndex;
    out.sampledPlayheadMs = m_clock.samplePlayheadMsForOutputTick(outputFrameIndex, state);

    QList<MediaVideoFrame> frames;
    for (int feed = 0; feed < m_feedCount; ++feed) {
        frames.append(cache.videoFrameOrPlaceholder(feed, out.sampledPlayheadMs));
    }
    out.video = Yuv420pCompositor::composeGrid(frames, m_width, m_height);
    out.video.ptsMs = out.sampledPlayheadMs;
    out.video.outputFrameIndex = outputFrameIndex;

    MediaAudioFrame audio;
    audio.feedIndex = -1;
    audio.startSample = sourceAudioStartSample(outputFrameIndex, state);
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm = silentS16Stereo(audioSamplesForOutputFrame(outputFrameIndex));
    out.audio = audio;
    return out;
}

OutputBusFrame OutputBusEngine::renderSingleSource(OutputBusId bus, int feedIndex,
                                                   qint64 outputFrameIndex,
                                                   const PlaybackStateSnapshot& state,
                                                   const OutputFrameCache& cache,
                                                   bool allowAudio) const {
    OutputBusFrame out;
    out.bus = bus;
    out.outputFrameIndex = outputFrameIndex;
    out.sampledPlayheadMs = m_clock.samplePlayheadMsForOutputTick(outputFrameIndex, state);

    if (feedIndex >= 0 && feedIndex < m_feedCount) {
        out.video = cache.videoFrameOrPlaceholder(feedIndex, out.sampledPlayheadMs);
    } else {
        out.video = MediaVideoFrame::solidYuv420p(m_width, m_height, 16, 128, 128);
        out.video.feedIndex = feedIndex;
        out.video.ptsMs = out.sampledPlayheadMs;
        out.video.isPlaceholder = true;
    }
    out.video.outputFrameIndex = outputFrameIndex;

    MediaAudioFrame audio;
    audio.feedIndex = feedIndex;
    audio.startSample = sourceAudioStartSample(outputFrameIndex, state);
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    const bool oneXForward = state.playing && state.speed > 0.99 && state.speed < 1.01;
    const int samples = audioSamplesForOutputFrame(outputFrameIndex);
    audio.pcm = (allowAudio && oneXForward && feedIndex >= 0 && feedIndex < m_feedCount)
                    ? cache.audioSpanOrSilence(feedIndex, audio.startSample, samples)
                    : silentS16Stereo(samples);
    out.audio = audio;
    return out;
}

qint64 OutputBusEngine::audioBoundarySampleForFrame(qint64 outputFrameIndex) const {
    if (outputFrameIndex <= 0) return 0;
    const FrameRate rate = m_clock.frameRate();
    if (!rate.isValid()) return outputFrameIndex * qint64(1600);
    return (outputFrameIndex * qint64(48000) * rate.denominator) / rate.numerator;
}

qint64 OutputBusEngine::sourceAudioStartSample(qint64 outputFrameIndex,
                                               const PlaybackStateSnapshot& state) const {
    const bool oneXForward = state.playing && state.speed > 0.99 && state.speed < 1.01;
    if (!oneXForward) return qMax<qint64>(0, state.playheadMs * qint64(48000) / 1000);

    const qint64 frameDelta = outputFrameIndex - state.playStartedAtOutputFrame;
    const qint64 baseSample = qMax<qint64>(0, state.playStartedAtPlayheadMs * qint64(48000) / 1000);
    return qMax<qint64>(0, baseSample + audioBoundarySampleForFrame(frameDelta));
}

int OutputBusEngine::audioSamplesForOutputFrame(qint64 outputFrameIndex) const {
    const qint64 start = audioBoundarySampleForFrame(outputFrameIndex);
    const qint64 end = audioBoundarySampleForFrame(outputFrameIndex + 1);
    return int(qMax<qint64>(0, end - start));
}
