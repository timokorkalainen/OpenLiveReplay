#ifndef LIVEFOLLOWPOLICY_H
#define LIVEFOLLOWPOLICY_H

#include <QtGlobal>

struct LiveFollowCorrection {
    bool adjustTransport = false;
    bool resetOutputClock = false;
    bool seekWorker = false;
    qint64 targetMs = 0;
    int directionHint = 0;
};

inline qint64 liveFollowTransportCorrectionThresholdMs(qint64 frameDurationMs) {
    return qMax<qint64>(50, qMax<qint64>(1, frameDurationMs) * 2);
}

inline qint64 liveFollowWorkerSeekThresholdMs(qint64 frameDurationMs) {
    return qMax<qint64>(250, qMax<qint64>(1, frameDurationMs) * 8);
}

inline qint64 liveFollowEffectiveLiveEdgeMs(qint64 liveEdgeMs, qint64 committedVideoTailMs) {
    if (committedVideoTailMs < 0) return liveEdgeMs;
    return qMin(liveEdgeMs, committedVideoTailMs);
}

inline LiveFollowCorrection planLiveFollowCorrection(bool followLive, bool playing,
                                                     qint64 liveEdgeMs, qint64 liveBufferMs,
                                                     qint64 currentMs, qint64 frameDurationMs,
                                                     qint64 committedVideoTailMs = -1) {
    LiveFollowCorrection correction;
    if (!followLive || !playing) return correction;

    const qint64 effectiveLiveEdgeMs =
        liveFollowEffectiveLiveEdgeMs(liveEdgeMs, committedVideoTailMs);
    correction.targetMs = qMax<qint64>(0, effectiveLiveEdgeMs - qMax<qint64>(0, liveBufferMs));
    if (correction.targetMs == 0 && effectiveLiveEdgeMs <= liveBufferMs) return correction;

    const qint64 delta = correction.targetMs - currentMs;
    const qint64 absDelta = qAbs(delta);
    if (absDelta <= liveFollowTransportCorrectionThresholdMs(frameDurationMs)) return correction;

    correction.adjustTransport = true;
    correction.directionHint = delta > 0 ? 1 : -1;
    if (absDelta >= liveFollowWorkerSeekThresholdMs(frameDurationMs))
        correction.seekWorker = true;
    else
        correction.resetOutputClock = true;
    return correction;
}

#endif // LIVEFOLLOWPOLICY_H
