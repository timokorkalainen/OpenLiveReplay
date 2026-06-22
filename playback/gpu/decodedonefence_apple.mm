#include "playback/gpu/decodedonefence.h"

#ifdef __APPLE__

#include <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <memory>
#include <mutex>
#include <thread>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {

class MetalDecodeDoneFence final : public DecodeDoneFence {
public:
    explicit MetalDecodeDoneFence(id<MTLSharedEvent> event) : m_event(event) {
        m_listener =
            [[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_queue_create(
                                                "net.openlivereplay.gpu.decode-fence",
                                                DISPATCH_QUEUE_SERIAL)];
    }
    ~MetalDecodeDoneFence() override {
        [m_listener release];
        [m_event release];
        m_listener = nil;
        m_event = nil;
    }

    void signalDecodeDone() override {
        const uint64_t next = m_target.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_event.signaledValue = next;
    }

    bool waitDecodeDone(int timeoutMs) override {
        if (signaledValue() == 0) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (signaledValue() == 0) {
                if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return waitForValue(signaledValue(), timeoutMs);
    }

    bool isSignaled() const override { return signaledValue() >= 1; }

    uint64_t signaledValue() const override {
        return m_target.load(std::memory_order_acquire);
    }

    bool waitForValue(uint64_t value, int timeoutMs) override {
        if (value == 0) return true;
        if (m_event.signaledValue >= value) return true;

        struct WaitState {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto state = std::make_shared<WaitState>();
        [m_event notifyListener:m_listener
                        atValue:value
                          block:^(__unused id<MTLSharedEvent> event,
                                  __unused uint64_t signaledValue) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->done = true;
                            state->cv.notify_all();
                          }];

        std::unique_lock<std::mutex> lock(state->mutex);
        if (timeoutMs < 0) {
            state->cv.wait(lock, [&] { return state->done; });
            return true;
        }
        return state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [&] { return state->done; });
    }

private:
    id<MTLSharedEvent> m_event = nil;
    MTLSharedEventListener* m_listener = nil;
    std::atomic<uint64_t> m_target{0};
};

} // namespace

std::shared_ptr<DecodeDoneFence> DecodeDoneFence::create() {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nullptr;

    id<MTLSharedEvent> event = [device newSharedEvent];
    [device release];
    if (!event) return nullptr;
    return std::make_shared<MetalDecodeDoneFence>(event);
}

#endif // __APPLE__
