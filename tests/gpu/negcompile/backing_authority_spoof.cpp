#include "playback/gpu/gpusurfacelease.h"

struct AppleSurfaceBackingAccess {
    static void* steal(const GpuReadLease& lease) { return lease.nativeHandleForBackend(); }
};

void* forbiddenBackingAuthoritySpoof(const GpuReadLease& lease) {
    return AppleSurfaceBackingAccess::steal(lease);
}
