#include "playback/gpu/gpusurfacelease.h"

#include <memory>

// Must NOT compile: read() is private; withRead() is the only public entry point.
void misuse(const std::shared_ptr<GpuSurface>& surface) {
    GpuSyncReadScope scope;
    const GpuReadLease lease = scope.read(surface);
    (void) lease;
}
