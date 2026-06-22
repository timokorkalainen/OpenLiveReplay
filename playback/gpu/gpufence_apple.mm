#include "playback/gpu/gpufence.h"

#ifdef __APPLE__

#include "playback/gpu/decodedonefence.h"

#include <utility>

GpuFence::~GpuFence() = default;

namespace {

class MetalGpuFence final : public GpuFence {
public:
    explicit MetalGpuFence(std::shared_ptr<DecodeDoneFence> fence) : m_fence(std::move(fence)) {}

    uint64_t signal() override {
        m_fence->signalDecodeDone();
        return m_fence->signaledValue();
    }

    bool wait(uint64_t value, int timeoutMs) override { return m_fence->waitForValue(value, timeoutMs); }

    uint64_t completedValue() const override { return m_fence->signaledValue(); }

private:
    std::shared_ptr<DecodeDoneFence> m_fence;
};

} // namespace

std::shared_ptr<GpuFence> GpuFence::create() {
    auto fence = DecodeDoneFence::create();
    if (!fence) return nullptr;
    return std::make_shared<MetalGpuFence>(std::move(fence));
}

#endif // __APPLE__
