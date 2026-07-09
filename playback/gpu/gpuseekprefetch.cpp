#include "playback/gpu/gpuseekprefetch.h"

#include <algorithm>

GpuPrefetchPlan GpuSeekPrefetch::planPrefetch(int64_t targetMs, int dir, int64_t frameDurMs,
                                              int64_t leadMs, qint64 surfaceBytes,
                                              qint64 budgetBytes, qint64 liveBytes) {
    GpuPrefetchPlan plan;
    if (frameDurMs <= 0 || leadMs <= 0 || surfaceBytes <= 0) return plan;

    const qint64 headroomBytes = budgetBytes - liveBytes;
    const int headroomSurfaces = headroomBytes > 0 ? int(headroomBytes / surfaceBytes) : 0;
    if (headroomSurfaces <= 0) return plan;

    const int64_t windowFrames = (leadMs + frameDurMs - 1) / frameDurMs;
    const int want = int(std::min<int64_t>(windowFrames, headroomSurfaces));
    if (want <= 0) return plan;

    if (dir >= 0) {
        plan.startMs = targetMs;
        plan.endMs = targetMs + leadMs;
    } else {
        plan.startMs = targetMs - leadMs;
        plan.endMs = targetMs;
    }
    plan.surfaceCount = want;
    return plan;
}
