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

inline LiveFollowCorrection planLiveFollowCorrection(bool followLive, bool playing,
                                                     qint64 liveEdgeMs, qint64 liveBufferMs,
                                                     qint64 currentMs, qint64 frameDurationMs) {
    LiveFollowCorrection correction;
    if (!followLive || !playing) return correction;

    correction.targetMs = qMax<qint64>(0, liveEdgeMs - qMax<qint64>(0, liveBufferMs));
    if (correction.targetMs == 0 && liveEdgeMs < liveBufferMs) return correction;

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
