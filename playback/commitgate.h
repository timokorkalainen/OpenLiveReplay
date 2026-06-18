#ifndef COMMITGATE_H
#define COMMITGATE_H

#include <cstdint>

// Pure seek-gate decision: no stored state, the worker owns the atomics.
namespace CommitGate {
// committedGen == seekGen  -> no reposition outstanding -> expose the live
//                            transport playhead (1x playback advances).
// committedGen != seekGen  -> a seek is in flight against a not-yet-ready
//                            cache -> hold the last committed playhead.
inline int64_t visiblePlayheadMs(int64_t transportPlayheadMs, int64_t committedPlayheadMs,
                                 uint64_t committedGen, uint64_t seekGen) {
    if (committedGen != seekGen) return committedPlayheadMs;
    return transportPlayheadMs;
}
} // namespace CommitGate

#endif // COMMITGATE_H
