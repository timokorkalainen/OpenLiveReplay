#ifndef OLR_GPU_SUBMISSION_H
#define OLR_GPU_SUBMISSION_H

#include <cstdint>
#include <memory>

class GpuFence;

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

struct GpuRetirementTicket {
    std::shared_ptr<GpuFence> fence;
    GpuFenceIdentity identity;
    uint64_t gpuGeneration = 0;
    uint64_t value = 0;
};

struct GpuSurfaceCompatibility {
    uintptr_t deviceDomainId = 0;
    uint64_t authorityEpoch = 0;
};

inline bool gpuSubmissionEvidenceMatches(const GpuSurfaceCompatibility& surface,
                                         const GpuFenceIdentity& fence,
                                         uint64_t submissionGeneration,
                                         uint64_t currentGeneration) noexcept {
    return surface.deviceDomainId != 0 && surface.authorityEpoch != 0 && fence.instanceId != 0 &&
           fence.deviceDomainId == surface.deviceDomainId &&
           fence.authorityEpoch == surface.authorityEpoch && submissionGeneration != 0 &&
           submissionGeneration == currentGeneration;
}

inline bool gpuExactTicketEvidenceMatches(const GpuRetirementTicket& ticket,
                                          const GpuFenceIdentity& submittingFence,
                                          const GpuSurfaceCompatibility& surface,
                                          uint64_t currentGeneration) noexcept {
    return ticket.fence && ticket.identity == submittingFence &&
           gpuSubmissionEvidenceMatches(surface, ticket.identity, ticket.gpuGeneration,
                                        currentGeneration);
}

inline bool gpuPreparedSubmissionEvidenceMatches(const GpuRetirementTicket& ticket,
                                                 const GpuFenceIdentity& submittingFence,
                                                 const GpuSurfaceCompatibility& surface,
                                                 uint64_t currentGeneration) noexcept {
    return ticket.value == 0 &&
           gpuExactTicketEvidenceMatches(ticket, submittingFence, surface, currentGeneration);
}

inline bool gpuRetirementEvidenceMatches(const GpuRetirementTicket& ticket,
                                         const GpuFenceIdentity& retiringFence,
                                         const GpuSurfaceCompatibility& surface,
                                         uint64_t currentGeneration) noexcept {
    return ticket.value != 0 &&
           gpuExactTicketEvidenceMatches(ticket, retiringFence, surface, currentGeneration);
}

#endif // OLR_GPU_SUBMISSION_H
