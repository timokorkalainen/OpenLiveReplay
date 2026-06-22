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

inline int64_t bookmarkedVisiblePlayheadMs(int64_t currentBookmarkMs, int64_t visiblePlayheadMs,
                                           int64_t cacheCoveredPlayheadMs, bool cacheCovered,
                                           uint64_t committedGen, uint64_t seekGen) {
    if (committedGen != seekGen || !cacheCovered) return currentBookmarkMs;
    if (cacheCoveredPlayheadMs > visiblePlayheadMs) return currentBookmarkMs;
    return cacheCoveredPlayheadMs;
}

inline int64_t cacheGuardedVisiblePlayheadMs(int64_t visiblePlayheadMs,
                                             int64_t bookmarkedPlayheadMs, bool cacheCovered,
                                             uint64_t committedGen, uint64_t seekGen) {
    if (committedGen == seekGen && !cacheCovered) return bookmarkedPlayheadMs;
    return visiblePlayheadMs;
}

inline bool canCommitReposition(uint64_t startedSeekGen, uint64_t currentSeekGen,
                                bool supersededSeekPending) {
    return !supersededSeekPending && startedSeekGen == currentSeekGen;
}

inline bool shouldInvalidateGpuGenerationForReposition(uint64_t startedSeekGen,
                                                       uint64_t committedGen) {
    return startedSeekGen != committedGen;
}

template <typename CommitFn>
inline bool commitRepositionIfCurrent(uint64_t startedSeekGen, uint64_t currentSeekGen,
                                      bool supersededSeekPending, CommitFn commit) {
    if (!canCommitReposition(startedSeekGen, currentSeekGen, supersededSeekPending)) return false;
    commit();
    return true;
}
} // namespace CommitGate

#endif // COMMITGATE_H
