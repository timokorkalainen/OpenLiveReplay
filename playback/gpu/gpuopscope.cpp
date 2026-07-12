#include "playback/gpu/gpuopscope.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"

#include <atomic>
#include <cassert>
#include <limits>

namespace {

std::atomic<uint64_t>& spillAllocations() {
    static std::atomic<uint64_t> count{0};
    return count;
}

} // namespace

GpuOpScope::GpuOpScope(std::shared_ptr<GpuFence> fence, GpuRetireRegistry& registry)
    : m_fence(std::move(fence)), m_registry(registry) {
    assert(m_fence && "GpuOpScope requires a real fence");
}

GpuOpScope::~GpuOpScope() {
    clearTracked();
}

bool GpuOpScope::contains(const GpuSurface* surface) const {
    for (qsizetype i = 0; i < m_inlineCount; ++i) {
        if (m_inline[static_cast<size_t>(i)].get() == surface) return true;
    }
    for (const auto& tracked : m_overflow) {
        if (tracked.get() == surface) return true;
    }
    return false;
}

bool GpuOpScope::track(std::shared_ptr<GpuSurface> surface) {
    if (m_state != State::Open || !surface || contains(surface.get())) return false;
    if (m_inlineCount < kInlineSurfaceCapacity) {
        m_inline[static_cast<size_t>(m_inlineCount++)] = std::move(surface);
        return true;
    }
    if (m_overflow.isEmpty()) spillAllocations().fetch_add(1, std::memory_order_relaxed);
    m_overflow.append(std::move(surface));
    return true;
}

bool GpuOpScope::cancel() {
    if (m_state != State::Open) return false;
    m_state = State::Canceled;
    clearTracked();
    return true;
}

bool GpuOpScope::finalizeSubmitted(GpuSubmitOutcome outcome) {
    if (m_state != State::Open || !m_fence) return false;
    m_state = State::Finalized;
    const uint64_t fenceValue = m_fence->signal();
    m_fenceValue = fenceValue;
    if (fenceValue == 0) {
        m_registry.noteSignalFailure();
        GpuDeviceLossMonitor::instance().recordSubmissionFailure();
        retireTracked(std::numeric_limits<uint64_t>::max());
        return false;
    }
    retireTracked(fenceValue);
    if (outcome == GpuSubmitOutcome::SubmittedWithError) {
        GpuDeviceLossMonitor::instance().recordSubmissionFailure();
        return false;
    }
    return true;
}

void GpuOpScope::retireTracked(uint64_t fenceValue) {
    m_registry.registerRetireBatch(m_inline.data(), m_inlineCount, m_fence, fenceValue);
    if (!m_overflow.isEmpty())
        m_registry.registerRetireBatch(m_overflow.data(), m_overflow.size(), m_fence, fenceValue);
    m_inlineCount = 0;
    m_overflow.clear();
}

void GpuOpScope::clearTracked() {
    for (qsizetype i = 0; i < m_inlineCount; ++i) {
        m_inline[static_cast<size_t>(i)].reset();
    }
    m_inlineCount = 0;
    m_overflow.clear();
}

uint64_t GpuOpScope::spillAllocationCount() {
    return spillAllocations().load(std::memory_order_relaxed);
}
