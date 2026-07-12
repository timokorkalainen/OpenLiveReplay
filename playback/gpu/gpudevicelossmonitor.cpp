#include "playback/gpu/gpudevicelossmonitor.h"

#include "playback/gpu/gpugeneration.h"

GpuDeviceLossMonitor& GpuDeviceLossMonitor::instance() {
    static GpuDeviceLossMonitor monitor;
    return monitor;
}

bool GpuDeviceLossMonitor::isLost() const {
    return m_lost.load(std::memory_order_acquire);
}

uint64_t GpuDeviceLossMonitor::lossCount() const {
    return m_lossCount.load(std::memory_order_acquire);
}

uint64_t GpuDeviceLossMonitor::recordLoss() {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (m_lost.load(std::memory_order_acquire)) {
        return m_lossGeneration.load(std::memory_order_acquire);
    }

    const uint64_t generation = GpuGenerationCounter::instance().bump();
    m_lossGeneration.store(generation, std::memory_order_release);
    m_lossCount.fetch_add(1, std::memory_order_acq_rel);
    m_undrained.fetch_add(1, std::memory_order_acq_rel);
    m_lost.store(true, std::memory_order_release);
    return generation;
}

void GpuDeviceLossMonitor::markRealDeviceLoss(const DeadDeviceToken& token) {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (!m_realLossToken.has_value()) m_realLossToken = token; // first writer wins in-epoch
}

std::optional<DeadDeviceToken> GpuDeviceLossMonitor::realLossToken() const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    return m_realLossToken;
}

bool GpuDeviceLossMonitor::consumeLossEvent() {
    uint64_t pending = m_undrained.load(std::memory_order_acquire);
    while (pending > 0 &&
           !m_undrained.compare_exchange_weak(pending, pending - 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
    }
    return pending > 0;
}

void GpuDeviceLossMonitor::clearForRebuild() {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    m_lost.store(false, std::memory_order_release);
    m_lossGeneration.store(0, std::memory_order_release);
    m_realLossToken.reset();
}

void GpuDeviceLossMonitor::reset() {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    m_lost.store(false, std::memory_order_release);
    m_lossCount.store(0, std::memory_order_release);
    m_undrained.store(0, std::memory_order_release);
    m_lossGeneration.store(0, std::memory_order_release);
    m_realLossToken.reset();
}
