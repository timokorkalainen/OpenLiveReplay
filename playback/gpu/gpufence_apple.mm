#include "playback/gpu/gpufence.h"

#ifdef __APPLE__

#include <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <memory>
#include <mutex>

GpuFence::~GpuFence() = default;

namespace {

class MetalGpuFence final : public GpuFence {
public:
    MetalGpuFence(id<MTLSharedEvent> event, id<MTLCommandQueue> queue)
        : m_event([event retain]), m_queue([queue retain]) {
        m_listener =
            [[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_queue_create(
                                                "net.openlivereplay.gpu.render-fence",
                                                DISPATCH_QUEUE_SERIAL)];
    }

    ~MetalGpuFence() override {
        [m_listener release];
        [m_queue release];
        [m_event release];
        m_listener = nil;
        m_queue = nil;
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
    id<MTLCommandQueue> m_queue = nil;
    MTLSharedEventListener* m_listener = nil;
    std::mutex m_signalMutex;
    std::atomic<uint64_t> m_target{0};
};

} // namespace

std::shared_ptr<GpuFence> makeMetalGpuFence(void* metalCommandQueue) {
    id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue>>(metalCommandQueue);
    if (!queue) return nullptr;

    id<MTLDevice> device = queue.device;
    if (!device) return nullptr;

    id<MTLSharedEvent> event = [device newSharedEvent];
    if (!event) return nullptr;

    auto fence = std::make_shared<MetalGpuFence>(event, queue);
    [event release];
    return fence;
}

std::shared_ptr<GpuFence> GpuFence::create() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nullptr;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    [device release];
    if (!queue) {
        return nullptr;
    }
    auto fence = makeMetalGpuFence(queue);
    [queue release];
    return fence;
}

#endif // __APPLE__
