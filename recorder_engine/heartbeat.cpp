#include "heartbeat.h"

#include <algorithm>

FrameSpan heartbeatFrameSpan(int64_t elapsedMs, const FrameRate& rate, int64_t lastFrame,
                             int maxPerTick, int64_t maxBacklogFrames) {
    FrameSpan span;
    span.from = lastFrame + 1;
    span.to = lastFrame; // empty until we know there is at least one frame to emit
    if (!rate.isValid()) {
        return span;
    }
    const int64_t derivedFrame = rate.frameForMs(elapsedMs);
    if (derivedFrame <= lastFrame) {
        return span; // frame count has not advanced
    }
    int64_t from = lastFrame + 1;
    if (maxBacklogFrames > 0 && derivedFrame - from >= maxBacklogFrames) {
        from = derivedFrame - maxBacklogFrames + 1; // resume near real time
    }
    const int64_t cap = std::max<int64_t>(1, maxPerTick);
    span.from = from;
    span.to = std::min<int64_t>(derivedFrame, from + cap - 1);
    return span;
}
