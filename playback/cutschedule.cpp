#include "playback/cutschedule.h"

#include <cmath>

qint64 CutSchedule::outputFrameForCut(qint64 nextOutputFrameIndex, int leadFrames) {
    if (leadFrames < 0) {
        leadFrames = 0;
    }
    return nextOutputFrameIndex + leadFrames;
}

qint64 CutSchedule::framesForLeadMs(qint64 leadMs, double fps) {
    if (leadMs <= 0 || fps <= 0.0) {
        return 0;
    }
    const double frames = (static_cast<double>(leadMs) * fps) / 1000.0;
    return static_cast<qint64>(std::ceil(frames));
}

bool CutSchedule::shouldFireAt(qint64 nextOutputFrameIndex, qint64 scheduledFrame) {
    return nextOutputFrameIndex >= scheduledFrame;
}

qint64 CutSchedule::playheadAfterCut(qint64 targetMs, qint64 firedFrameIndex,
                                     qint64 scheduledFrameIndex, double fps) {
    if (fps <= 0.0 || firedFrameIndex <= scheduledFrameIndex) {
        return targetMs;
    }
    const qint64 overshootFrames = firedFrameIndex - scheduledFrameIndex;
    const double msPerFrame = 1000.0 / fps;
    return targetMs + static_cast<qint64>(overshootFrames * msPerFrame);
}
