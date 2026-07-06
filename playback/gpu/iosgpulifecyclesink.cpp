#include "playback/gpu/iosgpulifecyclesink.h"

#include "playback/gpu/gpudevicelossmonitor.h"

IosGpuLifecycleSink::~IosGpuLifecycleSink() = default;

void DefaultIosGpuLifecycleSink::onEnterBackground() {
    m_suspended.store(true, std::memory_order_release);
    const uint64_t generation = GpuDeviceLossMonitor::instance().recordLoss();
    m_bgGeneration.store(generation, std::memory_order_release);
}

void DefaultIosGpuLifecycleSink::onEnterForeground() {
    m_suspended.store(false, std::memory_order_release);
}

bool DefaultIosGpuLifecycleSink::isSuspended() const {
    return m_suspended.load(std::memory_order_acquire);
}

uint64_t DefaultIosGpuLifecycleSink::generationAtLastBackground() const {
    return m_bgGeneration.load(std::memory_order_acquire);
}

namespace {

DefaultIosGpuLifecycleSink g_defaultSink;
std::atomic<IosGpuLifecycleSink*> g_sink{&g_defaultSink};

} // namespace

void setIosGpuLifecycleSink(IosGpuLifecycleSink* sink) {
    g_sink.store(sink ? sink : static_cast<IosGpuLifecycleSink*>(&g_defaultSink),
                 std::memory_order_release);
}

IosGpuLifecycleSink* iosGpuLifecycleSink() {
    return g_sink.load(std::memory_order_acquire);
}
