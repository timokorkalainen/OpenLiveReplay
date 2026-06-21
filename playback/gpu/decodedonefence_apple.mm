#include "playback/gpu/decodedonefence.h"

#ifdef __APPLE__

#include <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <thread>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {

class MetalDecodeDoneFence final : public DecodeDoneFence {
public:
    explicit MetalDecodeDoneFence(id<MTLSharedEvent> event) : m_event(event) {}
    ~MetalDecodeDoneFence() override {
        [m_event release];
        m_event = nil;
    }

    void signalDecodeDone() override {
        m_event.signaledValue = 1;
        m_signaled.store(true, std::memory_order_release);
    }

    bool waitDecodeDone(int timeoutMs) override {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!m_signaled.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    bool isSignaled() const override { return m_signaled.load(std::memory_order_acquire); }

private:
    id<MTLSharedEvent> m_event = nil;
    std::atomic<bool> m_signaled{false};
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
