#ifndef OLR_GPUFENCE_H
#define OLR_GPUFENCE_H

#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"

#include <cstdint>
#include <memory>

class GpuFence {
public:
    virtual ~GpuFence();

    virtual uint64_t signal() = 0;
    // timeoutMs < 0 waits indefinitely.
    virtual bool wait(uint64_t value, int timeoutMs) = 0;
    virtual uint64_t completedValue() const = 0;
    // Stable identity of the device timeline serviced by this fence. Retire
    // abandonment is scoped to this domain so loss of one context cannot free
    // resources still in flight on another live device.
    virtual uintptr_t deviceDomainId() const { return m_identity.deviceDomainId; }
    GpuFenceIdentity identity() const noexcept { return m_identity; }
    bool acceptsSubmission(const GpuSurfaceCompatibility& surface,
                           uint64_t gpuGeneration) const noexcept;
    bool validatesPreparedSubmission(const GpuRetirementTicket& ticket,
                                     const GpuSurfaceCompatibility& surface) const noexcept;
    bool validatesRetirement(const GpuRetirementTicket& ticket,
                             const GpuSurfaceCompatibility& surface) const noexcept;
    bool isCompatibleWith(const std::shared_ptr<GpuSurface>& surface) const {
        if (!surface) return false;
        const GpuSurfaceCompatibility exact = surface->compatibility();
        if (exact.deviceDomainId != 0 || exact.authorityEpoch != 0)
            return acceptsSubmission(exact, currentGpuGeneration());
        GpuSyncReadScope scope;
        const GpuReadLease lease = scope.read(surface);
        const bool compatible = isCompatibleWithNativeHandle(lease.nativeHandle());
        scope.complete();
        return compatible;
    }

    static std::shared_ptr<GpuFence> create();

protected:
    explicit GpuFence(uintptr_t deviceDomainId = 0, uint64_t authorityEpoch = 0);
    virtual bool isCompatibleWithNativeHandle(void* nativeHandle) const {
        return nativeHandle == nullptr;
    }

private:
    static uint64_t currentGpuGeneration() noexcept;

    GpuFenceIdentity m_identity;
};

#ifdef __APPLE__
std::shared_ptr<GpuFence> makeMetalGpuFence(void* metalCommandQueue);
#endif

#ifdef _WIN32
std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device);
#endif

#endif // OLR_GPUFENCE_H
