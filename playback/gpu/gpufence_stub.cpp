#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"

#if !defined(__APPLE__) && !defined(_WIN32)

#include <atomic>
#include <chrono>
#include <thread>

namespace {

std::atomic<uint64_t> nextFenceInstanceId{1};
const int stubDeviceDomain = 0;

class StubGpuFence final : public GpuFence {
public:
    StubGpuFence() : GpuFence(reinterpret_cast<uintptr_t>(&stubDeviceDomain)) {}
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

GpuFence::GpuFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
    : m_identity{nextFenceInstanceId.fetch_add(1, std::memory_order_relaxed), deviceDomainId,
                 authorityEpoch != 0 ? authorityEpoch
                                     : GpuGenerationCounter::instance().current()} {}

GpuFence::~GpuFence() = default;

uint64_t GpuFence::currentGpuGeneration() noexcept {
    return GpuGenerationCounter::instance().current();
}

bool GpuFence::acceptsSubmission(const GpuSurfaceCompatibility& surface,
                                 uint64_t gpuGeneration) const noexcept {
    return gpuSubmissionEvidenceMatches(surface, identity(), gpuGeneration, currentGpuGeneration());
}

bool GpuFence::validatesPreparedSubmission(const GpuRetirementTicket& ticket,
                                           const GpuSurfaceCompatibility& surface) const noexcept {
    return ticket.fence.get() == this && gpuPreparedSubmissionEvidenceMatches(
                                             ticket, identity(), surface, currentGpuGeneration());
}

bool GpuFence::validatesRetirement(const GpuRetirementTicket& ticket,
                                   const GpuSurfaceCompatibility& surface) const noexcept {
    return ticket.fence.get() == this &&
           gpuRetirementEvidenceMatches(ticket, identity(), surface, currentGpuGeneration());
}

std::shared_ptr<GpuFence> GpuFence::create() {
    return std::make_shared<StubGpuFence>();
}

#endif // !__APPLE__ && !_WIN32
