#include "playback/output/outputbusengine.h"

#include "playback/output/yuv420pcompositor.h"

#include <QList>

namespace {

constexpr quint32 kFnvOffset = 2166136261u;
constexpr quint32 kFnvPrime = 16777619u;

quint32 hashInt(quint32 hash, qint64 value) {
    for (int i = 0; i < 8; ++i) {
        hash ^= quint8((quint64(value) >> (i * 8)) & 0xff);
        hash *= kFnvPrime;
    }
    return hash;
}

// Programme timecode in 100 ns units from the playhead (1 ms = 10000 x 100 ns). Clamped at
// zero so a pre-start/negative playhead still yields a valid (non-sentinel) timecode.
qint64 programmeTimecode100nsFor(qint64 sampledPlayheadMs) {
    return qMax<qint64>(0, sampledPlayheadMs) * 10000;
}

FrameHandle placeholderVideoFrame(int feedIndex, qint64 playheadMs, int width, int height) {
    FrameHandle placeholder = solidYuv420pHandle(width, height, 16, 128, 128);
    placeholder.metadata().key.feedIndex = feedIndex;
    placeholder.metadata().key.ptsMs = playheadMs;
    placeholder.metadata().key.isPlaceholder = true;
    return placeholder;
}

std::optional<FrameHandle> freshVideoFrameAt(const OutputFrameCache& cache, int feedIndex,
                                             qint64 playheadMs, uint64_t gpuGeneration) {
    return cache.videoFrameAtFreshForGeneration(feedIndex, playheadMs, gpuGeneration);
}

FrameHandle freshVideoFrameOrPlaceholder(const OutputFrameCache& cache, int feedIndex,
                                         qint64 playheadMs, int width, int height,
                                         uint64_t gpuGeneration) {
    if (std::optional<FrameHandle> frame =
            freshVideoFrameAt(cache, feedIndex, playheadMs, gpuGeneration)) {
        return *frame;
    }
    return placeholderVideoFrame(feedIndex, playheadMs, width, height);
}

bool isSilentAudio(const MediaAudioFrame& audio) {
    for (const char sample : audio.pcm) {
        if (sample != '\0') return false;
    }
    return true;
}

// Identity is derived from frame metadata, not from the pixel/PCM payload. A cached
// source frame's content is uniquely keyed by (feedIndex, ptsMs), so hashing every plane
// byte every tick on the cadence thread (~3 MB per 1080p frame) is pure overhead. Keeping
// the hash metadata-only lets repeated-payload detection stay correct while removing the
// per-frame O(pixels) cost. Multiview overrides videoHash with its source-content
// signature (see renderMultiview) because its composed buffer has no single source pts.
quint32 videoHashFor(const FramePayloadKey& key) {
    quint32 hash = kFnvOffset;
    hash = hashInt(hash, key.feedIndex);
    hash = hashInt(hash, key.ptsMs);
    hash = hashInt(hash, key.width);
    hash = hashInt(hash, key.height);
    hash = hashInt(hash, int(key.format));
    hash = hashInt(hash, key.isPlaceholder ? 1 : 0);
    return hash;
}

quint32 audioHashFor(const MediaAudioFrame& audio) {
    quint32 hash = kFnvOffset;
    hash = hashInt(hash, audio.feedIndex);
    hash = hashInt(hash, audio.startSample);
    hash = hashInt(hash, audio.sampleRate);
    hash = hashInt(hash, audio.channels);
    hash = hashInt(hash, int(audio.format));
    hash = hashInt(hash, audio.sampleFrames());
    return hash;
}

} // namespace

QDebug operator<<(QDebug debug, const OutputFrameIdentity& identity) {
    QDebugStateSaver saver(debug);
    debug.nospace() << "OutputFrameIdentity(bus=" << int(identity.bus.kind) << ':'
                    << identity.bus.index << ", frame=" << identity.outputFrameIndex
                    << ", playhead=" << identity.sampledPlayheadMs
                    << ", sourceFeed=" << identity.sourceFeedIndex
                    << ", sourcePts=" << identity.sourcePtsMs
                    << ", placeholder=" << identity.videoPlaceholder
                    << ", audioSilent=" << identity.audioSilent
                    << ", videoHash=" << identity.videoHash << ", audioHash=" << identity.audioHash
                    << ", gpuGeneration=" << identity.videoGpuGeneration << ')';
    return debug;
}

OutputFrameIdentity outputFrameIdentityFor(const OutputBusFrame& frame) {
    const FramePayloadKey& videoKey = frame.video.metadata().key;
    OutputFrameIdentity identity;
    identity.bus = frame.bus;
    identity.outputFrameIndex = frame.outputFrameIndex;
    identity.sampledPlayheadMs = frame.sampledPlayheadMs;
    identity.sourceFeedIndex = videoKey.feedIndex;
    identity.sourcePtsMs = videoKey.ptsMs;
    identity.videoPlaceholder = videoKey.isPlaceholder;
    identity.audioSilent = isSilentAudio(frame.audio);
    identity.videoHash = videoHashFor(videoKey);
    identity.audioHash = audioHashFor(frame.audio);
    identity.videoGpuGeneration = frame.video.metadata().gpuGeneration;
    return identity;
}

OutputBusEngine::OutputBusEngine(FrameRate rate, int feedCount, int width, int height)
    : m_clock(rate), m_feedCount(qMax(0, feedCount)), m_width(qMax(2, width)),
      m_height(qMax(2, height)) {}

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
                                                const OutputFrameCache& cache,
                                                MultiviewComposite* memo) const {
    constexpr qint64 kAbsentFeedPts = -1;
    OutputBusFrame out;
    out.bus = OutputBusId::multiview();
    out.outputFrameIndex = outputFrameIndex;
    out.sampledPlayheadMs = m_clock.samplePlayheadMsForOutputTick(outputFrameIndex, state);
    out.programmeTimecode100ns = programmeTimecode100nsFor(out.sampledPlayheadMs);

    // Describe the source set selected for this tick. Absent feeds use a stable sentinel
    // (not the playhead) plus a present flag, so the descriptor is stable across ticks while
    // the selected source frames are unchanged. The descriptor drives the composite memo
    // exactly; the folded 32-bit signature drives repeated-payload identity (where a rare
    // hash collision only miscounts a stat, never produces wrong pixels).
    quint32 sourceSignature = kFnvOffset;
    QVector<qint64> sourceKeys;
    sourceKeys.reserve(static_cast<qsizetype>(m_feedCount) * 3);
    QVector<std::optional<FrameHandle>> sources;
    sources.reserve(m_feedCount);
    qint64 sourcePtsMs = 0;
    uint64_t sourceGpuGeneration = 0;
    bool anySourcePresent = false;
    for (int feed = 0; feed < m_feedCount; ++feed) {
        const std::optional<FrameHandle> src =
            freshVideoFrameAt(cache, feed, out.sampledPlayheadMs, state.gpuGeneration);
        sources.append(src);
        const qint64 pts = src ? src->metadata().key.ptsMs : kAbsentFeedPts;
        const uint64_t generation = src ? src->metadata().gpuGeneration : uint64_t(0);
        sourceKeys.append(src ? 1 : 0);
        sourceKeys.append(pts);
        sourceKeys.append(qint64(generation));
        sourceSignature = hashInt(sourceSignature, feed);
        sourceSignature = hashInt(sourceSignature, pts);
        sourceSignature = hashInt(sourceSignature, qint64(generation));
        sourceSignature = hashInt(sourceSignature, src ? 0 : 1);
        if (src) {
            anySourcePresent = true;
            sourcePtsMs = qMax(sourcePtsMs, src->metadata().key.ptsMs);
            sourceGpuGeneration = qMax(sourceGpuGeneration, generation);
        }
    }

    if (memo && memo->valid && memo->sourceKeys == sourceKeys) {
        out.video = memo->video; // reuse composited planes (copy-on-write share)
    } else {
        QList<FrameHandle> frames;
        for (int feed = 0; feed < m_feedCount; ++feed) {
            const std::optional<FrameHandle>& source = sources.at(feed);
            if (source.has_value()) {
                frames.append(source.value());
            } else {
                frames.append(
                    placeholderVideoFrame(feed, out.sampledPlayheadMs, m_width, m_height));
            }
        }
        out.video = Yuv420pCompositor::composeGrid(frames, m_width, m_height);
        out.video.metadata().key.isPlaceholder = !anySourcePresent;
        if (memo) {
            memo->valid = true;
            memo->sourceKeys = sourceKeys;
            memo->video = out.video;
        }
    }

    // Identity must reflect the composited source content, not the advancing playhead,
    // so repeated-payload detection works when the underlying feeds are frozen.
    out.video.metadata().key.ptsMs = sourcePtsMs;
    out.video.metadata().gpuGeneration = sourceGpuGeneration;
    out.video.metadata().outputFrameIndex = outputFrameIndex;

    out.audio = renderAudioForFeed(state.selectedFeedIndex, outputFrameIndex, state, cache, true);
    out.identity = outputFrameIdentityFor(out);
    out.identity.videoHash = sourceSignature;
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
    out.programmeTimecode100ns = programmeTimecode100nsFor(out.sampledPlayheadMs);

    if (feedIndex >= 0 && feedIndex < m_feedCount) {
        out.video = freshVideoFrameOrPlaceholder(cache, feedIndex, out.sampledPlayheadMs, m_width,
                                                 m_height, state.gpuGeneration);
    } else {
        out.video = placeholderVideoFrame(feedIndex, out.sampledPlayheadMs, m_width, m_height);
    }
    out.video.metadata().outputFrameIndex = outputFrameIndex;

    out.audio = renderAudioForFeed(feedIndex, outputFrameIndex, state, cache, allowAudio);
    out.identity = outputFrameIdentityFor(out);
    return out;
}

MediaAudioFrame OutputBusEngine::renderAudioForFeed(int feedIndex, qint64 outputFrameIndex,
                                                    const PlaybackStateSnapshot& state,
                                                    const OutputFrameCache& cache,
                                                    bool allowAudio) const {
    MediaAudioFrame audio;
    audio.feedIndex = feedIndex;
    audio.startSample = sourceAudioStartSample(outputFrameIndex, state);
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    const bool oneXForward = state.playing && state.speed > 0.99 && state.speed < 1.01;
    if (allowAudio && oneXForward && feedIndex >= 0 && feedIndex < m_feedCount) {
        // Derive the per-frame sample count from the epoch-relative next start so the
        // count shares the same fractional-rate phase as the start. Otherwise, for an
        // odd play epoch at 29.97/59.94 the alternating 1601/1602 size disagrees with
        // the start step and consecutive spans overlap/gap by one sample forever.
        const qint64 nextStart = sourceAudioStartSample(outputFrameIndex + 1, state);
        const int samples = int(qMax<qint64>(0, nextStart - audio.startSample));
        audio.pcm = cache.audioSpanOrSilence(feedIndex, audio.startSample, samples);
    } else {
        audio.pcm = silentS16Stereo(audioSamplesForOutputFrame(outputFrameIndex));
    }
    return audio;
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
