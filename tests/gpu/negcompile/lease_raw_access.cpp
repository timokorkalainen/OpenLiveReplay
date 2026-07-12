#include "playback/gpu/gpusurfacelease.h"

void* forbiddenLeaseRawAccess(const GpuReadLease& lease) {
    return lease.nativeHandle();
}
