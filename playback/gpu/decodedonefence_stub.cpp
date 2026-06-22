#include "playback/gpu/decodedonefence.h"

#ifndef __APPLE__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {

class NoopDecodeDoneFence final : public DecodeDoneFence {
public:
    void signalDecodeDone() override { m_value.fetch_add(1, std::memory_order_acq_rel); }
    bool waitDecodeDone(int timeoutMs) override { return waitForValue(1, timeoutMs); }
    bool isSignaled() const override { return m_value.load(std::memory_order_acquire) >= 1; }
    uint64_t signaledValue() const override { return m_value.load(std::memory_order_acquire); }
    bool waitForValue(uint64_t value, int timeoutMs) override {
        if (value == 0) return true;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (m_value.load(std::memory_order_acquire) < value) {
            if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

private:
    std::atomic<uint64_t> m_value{0};
};

} // namespace

std::shared_ptr<DecodeDoneFence> DecodeDoneFence::create() {
    return std::make_shared<NoopDecodeDoneFence>();
}

#endif // !__APPLE__
