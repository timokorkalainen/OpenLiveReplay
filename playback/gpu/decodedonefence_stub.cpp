#include "playback/gpu/decodedonefence.h"

#ifndef __APPLE__

#include <atomic>
#include <memory>

DecodeDoneFence::~DecodeDoneFence() = default;

namespace {

class NoopDecodeDoneFence final : public DecodeDoneFence {
public:
    void signalDecodeDone() override { m_signaled.store(true, std::memory_order_release); }
    bool waitDecodeDone(int) override { return true; }
    bool isSignaled() const override { return m_signaled.load(std::memory_order_acquire); }

private:
    std::atomic<bool> m_signaled{false};
};

} // namespace

std::shared_ptr<DecodeDoneFence> DecodeDoneFence::create() {
    return std::make_shared<NoopDecodeDoneFence>();
}

#endif // !__APPLE__
