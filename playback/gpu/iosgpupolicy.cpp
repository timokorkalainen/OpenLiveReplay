#include "playback/gpu/iosgpupolicy.h"

#include <algorithm>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace {

int macosPerTrackCap(int trackCount) {
    const int n = trackCount > 0 ? trackCount : 1;
    return std::max(12, 256 / n);
}

} // namespace

bool gpuIsIosBuild() {
#if defined(__APPLE__) && TARGET_OS_IOS
    return true;
#else
    return false;
#endif
}

int gpuPerTrackWindowCap(int trackCount) {
    const int base = macosPerTrackCap(trackCount);
    if (gpuIsIosBuild()) return std::min(base, kIosMaxPerTrackGpuFrames);
    return base;
}

int gpuIosAggregateWindowCeiling() {
    return kIosAggregateGpuFrameCeiling;
}
