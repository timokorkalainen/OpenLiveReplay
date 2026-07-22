#include "playback/gpu/gpusurfacelease.h"

void forbiddenScopedNativeAutoEscape(const GpuScopedNativeView<1>& view) {
    auto escaped = view[0];
    (void) escaped;
}
