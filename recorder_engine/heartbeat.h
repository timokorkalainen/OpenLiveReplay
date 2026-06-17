#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <cstdint>

// Inclusive range of frame indices to emit on one heartbeat tick. Empty when to < from.
struct FrameSpan {
    int64_t from = 1;
    int64_t to = 0;
};

// Which frame indices a heartbeat tick should emit, given the wall-clock elapsed
// time since recording start. The recording timeline is wall-clock-derived: the
// target frame is elapsedMs*fps/1000. We emit lastFrame+1 .. target, but:
//   - skip ahead when more than maxBacklogFrames behind (a long stall resumes near
//     real time instead of replaying a huge backlog), and
//   - cap the burst at maxPerTick frames (the remainder drains on later ticks) so a
//     catch-up never freezes the caller's thread.
// Returns an empty span (to < from) when the frame count has not advanced or fps<=0.
// fps is the ONLY frame-rate dependence; P1 (rational fps) swaps it here alone.
FrameSpan heartbeatFrameSpan(int64_t elapsedMs, int fps, int64_t lastFrame, int maxPerTick,
                             int64_t maxBacklogFrames);

#endif // HEARTBEAT_H
