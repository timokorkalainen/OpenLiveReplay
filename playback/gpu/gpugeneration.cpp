#include "playback/gpu/gpugeneration.h"

GpuGenerationCounter& GpuGenerationCounter::instance() {
    static GpuGenerationCounter counter;
    return counter;
}

uint64_t GpuGenerationCounter::current() const {
    return m_generation.load(std::memory_order_acquire);
}

uint64_t GpuGenerationCounter::bump() {
    return m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void GpuGenerationCounter::reset() {
    m_generation.store(0, std::memory_order_release);
}
