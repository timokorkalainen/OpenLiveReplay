#include "playback/gpu/gpufence.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <atomic>
#include <chrono>
#include <thread>

GpuFence::~GpuFence() = default;

namespace {

class StubGpuFence final : public GpuFence {
public:
    uint64_t signal() override { return m_value.fetch_add(1, std::memory_order_acq_rel) + 1; }

    bool wait(uint64_t value, int timeoutMs) override {
        if (value == 0) return true;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (m_value.load(std::memory_order_acquire) < value) {
            if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    uint64_t completedValue() const override { return m_value.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> m_value{0};
};

} // namespace

std::shared_ptr<GpuFence> GpuFence::create() {
    return std::make_shared<StubGpuFence>();
}

#endif // !__APPLE__ && !_WIN32
