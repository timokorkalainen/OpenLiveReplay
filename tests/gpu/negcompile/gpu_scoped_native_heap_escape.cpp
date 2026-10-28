#include "playback/gpu/gpusurfacelease.h"

GpuScopedNativeSurface* forbiddenScopedNativeHeapEscape(const GpuScopedNativeView<1>& view) {
    return new GpuScopedNativeSurface(view[0]);
}
