#include "playback/gpu/gpuframereadbacktelemetry.h"

#include <atomic>

namespace {

std::atomic<qint64> s_frameReadToCpuCount{0};

} // namespace

qint64 gpuFrameReadToCpuCount() {
    return s_frameReadToCpuCount.load(std::memory_order_acquire);
}

void gpuResetFrameReadToCpuCount() {
    s_frameReadToCpuCount.store(0, std::memory_order_release);
}

void gpuRecordFrameReadToCpuReadback() {
    s_frameReadToCpuCount.fetch_add(1, std::memory_order_acq_rel);
}
