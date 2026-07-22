#ifndef OLR_GPUFENCE_H
#define OLR_GPUFENCE_H

#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

class GpuFence : public std::enable_shared_from_this<GpuFence> {
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
    bool validatesRetirement(const GpuRetirementTicket& ticket,
                             const GpuSurfaceCompatibility& surface) const noexcept;
    // Legacy device-authority probe only. This does not authorize submission or
    // mint retirement evidence; asynchronous paths use submitExactForRetirement().
    bool sharesDeviceAuthorityWith(const std::shared_ptr<GpuSurface>& surface) const {
        if (!surface) return false;
        const GpuSurfaceCompatibility exact = surface->compatibility();
        if (exact.deviceDomainId != 0 || exact.authorityEpoch != 0)
            return gpuSubmissionDetail::matchesSurfaceEvidence(
                exact, identity(), currentGpuGeneration(), currentGpuGeneration());
        bool compatible = false;
        GpuSyncReadScope scope;
        scope.withRead(surface, [&](const GpuReadLease& lease) {
            void* handle = lease.nativeHandle();
            compatible = isCompatibleWithNativeHandle(handle);
        });
        return compatible;
    }

    static std::shared_ptr<GpuFence> create();

protected:
    explicit GpuFence(uintptr_t deviceDomainId = 0, uint64_t authorityEpoch = 0);

    // Fused authority boundary: all evidence is checked before the callback can
    // reach a driver. Only this exact fence can then signal and mint the immutable
    // ticket carrying the value returned by that signal.
    template <typename SubmitFn>
    std::optional<GpuRetirementTicket>
    submitExactForRetirement(const GpuFenceIdentity& preparedFence,
                             const GpuSurfaceCompatibility& surface, uint64_t gpuGeneration,
                             SubmitFn&& submitFn) {
        static_assert(std::is_nothrow_invocable_r_v<bool, SubmitFn&&>,
                      "GPU submission callback must be noexcept and report driver acceptance");
        std::shared_ptr<GpuFence> owner = weak_from_this().lock();
        if (!owner || preparedFence != identity() ||
            !gpuSubmissionDetail::matchesSurfaceEvidence(surface, preparedFence, gpuGeneration,
                                                         currentGpuGeneration()))
            return std::nullopt;

        if (!std::invoke(std::forward<SubmitFn>(submitFn))) return std::nullopt;
        const uint64_t value = signal();
        if (value == 0) return std::nullopt;
        const uint64_t seal = sealTicket(preparedFence, gpuGeneration, value);
        return GpuRetirementTicket(std::move(owner), preparedFence, gpuGeneration, value, seal);
    }

    virtual bool isCompatibleWithNativeHandle(void* handle) const { return handle == nullptr; }

private:
    friend class GpuOpScope;
    // Post-accept validation for the exact ticket this fence just minted. A
    // generation change may race after the driver accepts work; that work must
    // still retain its owners until this fence retires even though downstream
    // consumers will drop the now-stale frame generation.
    bool validatesIssuedRetirement(const GpuRetirementTicket& ticket,
                                   const GpuSurfaceCompatibility& surface) const noexcept {
        return ticket.m_fence.get() == this && ticket.m_identity == identity() &&
               ticket.m_identity.instanceId != 0 && ticket.m_value != 0 &&
               ticket.m_gpuGeneration != 0 && surface.deviceDomainId != 0 &&
               surface.authorityEpoch != 0 &&
               ticket.m_identity.deviceDomainId == surface.deviceDomainId &&
               ticket.m_identity.authorityEpoch == surface.authorityEpoch &&
               ticket.m_authoritySeal ==
                   sealTicket(ticket.m_identity, ticket.m_gpuGeneration, ticket.m_value);
    }
    static uint64_t currentGpuGeneration() noexcept;
    static uint64_t mixAuthorityWord(uint64_t state, uint64_t word) noexcept {
        state ^= word + 0x9e3779b97f4a7c15ULL + (state << 6) + (state >> 2);
        state ^= state >> 30;
        state *= 0xbf58476d1ce4e5b9ULL;
        state ^= state >> 27;
        state *= 0x94d049bb133111ebULL;
        return state ^ (state >> 31);
    }
    static uint64_t makeTicketAuthorityKey(const GpuFenceIdentity& identity,
                                           const GpuFence* fence) noexcept {
        uint64_t state = mixAuthorityWord(identity.instanceId, identity.deviceDomainId);
        state = mixAuthorityWord(state, identity.authorityEpoch);
        return mixAuthorityWord(state, reinterpret_cast<uintptr_t>(fence));
    }
    uint64_t sealTicket(const GpuFenceIdentity& identity, uint64_t gpuGeneration,
                        uint64_t value) const noexcept {
        uint64_t state = mixAuthorityWord(m_ticketAuthorityKey, identity.instanceId);
        state = mixAuthorityWord(state, identity.deviceDomainId);
        state = mixAuthorityWord(state, identity.authorityEpoch);
        state = mixAuthorityWord(state, gpuGeneration);
        return mixAuthorityWord(state, value);
    }

    GpuFenceIdentity m_identity;
    uint64_t m_ticketAuthorityKey = 0;
};

#ifdef __APPLE__
uintptr_t gpuMetalDeviceDomainId(void* metalDevice);
std::shared_ptr<GpuFence> makeMetalGpuFence(void* metalCommandQueue, uint64_t authorityEpoch);
#endif

#ifdef _WIN32
std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device, uint64_t authorityEpoch);
#endif

#endif // OLR_GPUFENCE_H
