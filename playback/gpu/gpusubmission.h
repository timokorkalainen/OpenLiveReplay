#ifndef OLR_GPU_SUBMISSION_H
#define OLR_GPU_SUBMISSION_H

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

class GpuFence;
class GpuSurface;
#ifdef OLR_UNIT_TEST
struct GpuRetirementTicketTestAuthority;
#endif

// Exact identity of one fence timeline. The instance ID is process-monotonic and
// never derived from a driver object address. The domain and authority epoch bind
// that timeline to the device authority under which it was created.
struct GpuFenceIdentity {
    uint64_t instanceId = 0;
    uintptr_t deviceDomainId = 0;
    uint64_t authorityEpoch = 0;

    friend bool operator==(const GpuFenceIdentity& a, const GpuFenceIdentity& b) {
        return a.instanceId == b.instanceId && a.deviceDomainId == b.deviceDomainId &&
               a.authorityEpoch == b.authorityEpoch;
    }
    friend bool operator!=(const GpuFenceIdentity& a, const GpuFenceIdentity& b) {
        return !(a == b);
    }
};

struct GpuSurfaceCompatibility {
    uintptr_t deviceDomainId = 0;
    uint64_t authorityEpoch = 0;
};

enum class GpuSubmitOutcome : uint8_t { NotSubmitted, Submitted, SubmittedWithError };

enum class GpuRetirementDisposition : uint8_t { None, Published, Quarantined };

struct GpuSubmissionResult {
    GpuSubmitOutcome outcome = GpuSubmitOutcome::NotSubmitted;
    GpuRetirementDisposition retirement = GpuRetirementDisposition::None;
    uint64_t fenceValue = 0;

    bool driverAccepted() const noexcept { return outcome != GpuSubmitOutcome::NotSubmitted; }
    bool succeeded() const noexcept {
        return outcome == GpuSubmitOutcome::Submitted &&
               retirement == GpuRetirementDisposition::Published;
    }
};

template <size_t N>
class GpuSurfacePack final {
public:
    static_assert(N > 0, "A fused GPU submission must own at least one surface slot");

    explicit GpuSurfacePack(std::array<std::shared_ptr<GpuSurface>, N> surfaces) noexcept
        : m_surfaces(std::move(surfaces)) {}

    const std::array<std::shared_ptr<GpuSurface>, N>& owners() const noexcept { return m_surfaces; }

private:
    std::array<std::shared_ptr<GpuSurface>, N> m_surfaces;
};

// Immutable proof minted only after the exact fence authority accepts a fused
// submission callback and returns the signal value for that same timeline.
class GpuRetirementTicket final {
public:
    GpuRetirementTicket(const GpuRetirementTicket&) = default;
    GpuRetirementTicket(GpuRetirementTicket&&) noexcept = default;
    GpuRetirementTicket& operator=(const GpuRetirementTicket&) = delete;
    GpuRetirementTicket& operator=(GpuRetirementTicket&&) = delete;

    const std::shared_ptr<GpuFence>& fence() const noexcept { return m_fence; }
    GpuFenceIdentity identity() const noexcept { return m_identity; }
    uint64_t gpuGeneration() const noexcept { return m_gpuGeneration; }
    uint64_t value() const noexcept { return m_value; }

private:
    friend class GpuFence;
#ifdef OLR_UNIT_TEST
    friend struct GpuRetirementTicketTestAuthority;
#endif

    GpuRetirementTicket(std::shared_ptr<GpuFence> fence, GpuFenceIdentity identity,
                        uint64_t gpuGeneration, uint64_t value, uint64_t authoritySeal) noexcept
        : m_fence(std::move(fence)), m_identity(identity), m_gpuGeneration(gpuGeneration),
          m_value(value), m_authoritySeal(authoritySeal) {}

    std::shared_ptr<GpuFence> m_fence;
    GpuFenceIdentity m_identity;
    uint64_t m_gpuGeneration = 0;
    uint64_t m_value = 0;
    uint64_t m_authoritySeal = 0;
};

namespace gpuSubmissionDetail {

inline bool matchesSurfaceEvidence(const GpuSurfaceCompatibility& surface,
                                   const GpuFenceIdentity& fence, uint64_t submissionGeneration,
                                   uint64_t currentGeneration) noexcept {
    return surface.deviceDomainId != 0 && surface.authorityEpoch != 0 && fence.instanceId != 0 &&
           fence.deviceDomainId == surface.deviceDomainId &&
           fence.authorityEpoch == surface.authorityEpoch && submissionGeneration != 0 &&
           submissionGeneration == currentGeneration;
}

// `next == 0` is the permanent exhausted state. UINT64_MAX is issued once and
// atomically transitions the counter to exhausted; it can never wrap to 1.
inline uint64_t takeMonotonicInstanceId(std::atomic<uint64_t>& next) noexcept {
    uint64_t candidate = next.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == 0) return 0;
        const uint64_t successor =
            candidate == std::numeric_limits<uint64_t>::max() ? 0 : candidate + 1;
        if (next.compare_exchange_weak(candidate, successor, std::memory_order_relaxed,
                                       std::memory_order_relaxed))
            return candidate;
    }
}

} // namespace gpuSubmissionDetail

#endif // OLR_GPU_SUBMISSION_H
