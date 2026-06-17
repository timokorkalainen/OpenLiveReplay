#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <cstdint>

#include "framerate.h"

// Inclusive range of frame indices to emit on one heartbeat tick. Empty when to < from.
struct FrameSpan {
    int64_t from = 1;
    int64_t to = 0;
};

// Which frame indices a heartbeat tick should emit, given the wall-clock elapsed
// time since recording start. The recording timeline is wall-clock-derived: the
// target frame is rate.frameForMs(elapsedMs) (= elapsedMs*num/(1000*den)). We emit
// lastFrame+1 .. target, but:
//   - skip ahead when more than maxBacklogFrames behind (a long stall resumes near
//     real time instead of replaying a huge backlog), and
//   - cap the burst at maxPerTick frames (the remainder drains on later ticks) so a
//     catch-up never freezes the caller's thread.
// Returns an empty span (to < from) when the frame count has not advanced or the
// rate is invalid (!rate.isValid()).
// The frame rate enters the heartbeat only through `rate.frameForMs`.
FrameSpan heartbeatFrameSpan(int64_t elapsedMs, const FrameRate& rate, int64_t lastFrame,
                             int maxPerTick, int64_t maxBacklogFrames);

#endif // HEARTBEAT_H
