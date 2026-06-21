#include "playback/output/outputframeclock.h"

#include <QtGlobal>

OutputFrameClock::OutputFrameClock(FrameRate rate) : m_rate(rate) {}

qint64 OutputFrameClock::frameIndexToPlayheadMs(qint64 frameIndex) const {
    return m_rate.frameIndexToMs(frameIndex);
}

qint64 OutputFrameClock::samplePlayheadMsForOutputTick(qint64 outputFrameIndex,
                                                       const PlaybackStateSnapshot& state) const {
    if (!m_rate.isValid()) return qMax<qint64>(0, state.playheadMs);
    if (!state.playing) return qMax<qint64>(0, state.playheadMs);

    const qint64 frameDelta = outputFrameIndex - state.playStartedAtOutputFrame;
    const double mediaFrameDelta = double(frameDelta) * state.speed;
    const double msDelta =
        mediaFrameDelta * 1000.0 * double(m_rate.denominator) / double(m_rate.numerator);
    return qMax<qint64>(0, state.playStartedAtPlayheadMs + qint64(msDelta));
}

qint64 OutputFrameClock::outputFrameForPlayheadMs(qint64 playheadMs,
                                                  const PlaybackStateSnapshot& state) const {
    if (!m_rate.isValid() || !state.playing || state.speed <= 0.0) return -1;
    const double msPerMediaFrame = 1000.0 * double(m_rate.denominator) / double(m_rate.numerator);
    const double frameDelta =
        double(playheadMs - state.playStartedAtPlayheadMs) / (state.speed * msPerMediaFrame);
    return state.playStartedAtOutputFrame + qRound64(frameDelta);
}
