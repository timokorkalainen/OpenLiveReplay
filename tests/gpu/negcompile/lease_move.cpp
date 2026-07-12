#include "playback/gpu/gpusurfacelease.h"

#include <utility>

GpuReadLease forbiddenLeaseMove(GpuReadLease& lease) {
    return std::move(lease);
}
