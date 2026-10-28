#include "playback/gpu/gpusurfacelease.h"

GpuScopedNativeSurface forbiddenScopedNativeEscape(const GpuScopedNativeView<1>& view) {
    return view[0];
}
