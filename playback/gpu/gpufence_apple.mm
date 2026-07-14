#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"

#ifdef __APPLE__

#include <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <memory>
#include <mutex>

namespace {

std::atomic<uint64_t> nextFenceInstanceId{1};

class MetalGpuFence final : public GpuFence {
public:
    MetalGpuFence(id<MTLSharedEvent> event, id<MTLCommandQueue> queue, uint64_t authorityEpoch)
        : GpuFence(reinterpret_cast<uintptr_t>(queue.device), authorityEpoch),
          m_event([event retain]), m_device([queue.device retain]), m_queue([queue retain]) {
        m_listener = [[MTLSharedEventListener alloc]
            initWithDispatchQueue:dispatch_queue_create("net.openlivereplay.gpu.render-fence",
                                                        DISPATCH_QUEUE_SERIAL)];
    }

    ~MetalGpuFence() override {
        [m_listener release];
        [m_queue release];
        [m_device release];
        [m_event release];
        m_listener = nil;
        m_queue = nil;
        m_device = nil;
        m_event = nil;
    }

    uint64_t signal() override {
        if (!m_event || !m_queue) return 0;
        std::lock_guard<std::mutex> lock(m_signalMutex);
        id<MTLCommandBuffer> commandBuffer = [m_queue commandBuffer];
        if (!commandBuffer) return 0;

        const uint64_t value = m_target.fetch_add(1, std::memory_order_acq_rel) + 1;
        [commandBuffer encodeSignalEvent:m_event value:value];
        [commandBuffer commit];
        return value;
    }

    bool wait(uint64_t value, int timeoutMs) override {
        if (value == 0) return true;
        if (!m_event || !m_listener) return false;
        if (completedValue() >= value) return true;

        struct WaitState {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto state = std::make_shared<WaitState>();
        [m_event notifyListener:m_listener
                        atValue:value
                          block:^(id<MTLSharedEvent> event, uint64_t) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->done = event.signaledValue >= value;
                            state->cv.notify_all();
                          }];

        std::unique_lock<std::mutex> lock(state->mutex);
        if (m_event.signaledValue >= value) return true;
        if (timeoutMs < 0) {
            state->cv.wait(lock, [&] { return state->done; });
            return true;
        }
        return state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [&] { return state->done; });
    }

    uint64_t completedValue() const override {
        return m_event ? m_event.signaledValue : uint64_t(0);
    }

private:
    id<MTLSharedEvent> m_event = nil;
    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_queue = nil;
    MTLSharedEventListener* m_listener = nil;
    std::mutex m_signalMutex;
    std::atomic<uint64_t> m_target{0};
};

} // namespace

GpuFence::GpuFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
    : m_identity{gpuSubmissionDetail::takeMonotonicInstanceId(nextFenceInstanceId), deviceDomainId,
                 authorityEpoch != 0
                     ? authorityEpoch
                     : GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch()},
      m_ticketAuthorityKey(makeTicketAuthorityKey(m_identity, this)) {}

GpuFence::~GpuFence() = default;

uint64_t GpuFence::currentGpuGeneration() noexcept {
    return GpuGenerationCounter::instance().current();
}

bool GpuFence::validatesRetirement(const GpuRetirementTicket& ticket,
                                   const GpuSurfaceCompatibility& surface) const noexcept {
    return ticket.m_fence.get() == this && ticket.m_identity == identity() && ticket.m_value != 0 &&
           gpuSubmissionDetail::matchesSurfaceEvidence(
               surface, ticket.m_identity, ticket.m_gpuGeneration, currentGpuGeneration()) &&
           ticket.m_authoritySeal ==
               sealTicket(ticket.m_identity, ticket.m_gpuGeneration, ticket.m_value);
}

std::shared_ptr<GpuFence> makeMetalGpuFence(void* metalCommandQueue, uint64_t authorityEpoch) {
    id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue>>(metalCommandQueue);
    if (!queue || authorityEpoch == 0) return nullptr;

    id<MTLDevice> device = queue.device;
    if (!device) return nullptr;

    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!event) return nullptr;

    auto fence = std::make_shared<MetalGpuFence>(event, queue, authorityEpoch);
    [event release];
    return fence;
}

std::shared_ptr<GpuFence> GpuFence::create() {
    GpuDeviceLossMonitor& monitor = GpuDeviceLossMonitor::instance();
    const uint64_t authorityEpoch = monitor.currentDeviceAuthorityEpoch();
    if (authorityEpoch == 0) return nullptr;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nullptr;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    [device release];
    if (!queue) return nullptr;
    if (!monitor.isCurrentDeviceAuthority(authorityEpoch)) {
        [queue release];
        return nullptr;
    }

    auto fence = makeMetalGpuFence(queue, authorityEpoch);
    [queue release];
    return fence;
}

#endif // __APPLE__
