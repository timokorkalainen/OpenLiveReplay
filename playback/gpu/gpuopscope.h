#ifndef OLR_GPU_OP_SCOPE_H
#define OLR_GPU_OP_SCOPE_H

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusubmission.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

class GpuOpScope final {
public:
    GpuOpScope(std::shared_ptr<GpuFence> fence, GpuRetireRegistry& registry);
    ~GpuOpScope() = default;

    GpuOpScope(const GpuOpScope&) = delete;
    GpuOpScope& operator=(const GpuOpScope&) = delete;
    GpuOpScope(GpuOpScope&&) = delete;
    GpuOpScope& operator=(GpuOpScope&&) = delete;

    uint64_t fenceValue() const { return m_fenceValue; }

    template <typename BackendAdapter, size_t N>
    GpuSubmissionResult submit(BackendAdapter& adapter, GpuSurfacePack<N> surfaces) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<GpuSubmitOutcome, BackendAdapter&>,
                      "GPU backend adapters must be noexcept and return GpuSubmitOutcome");

        GpuSubmissionResult result;
        if (m_submitted || !m_fence) return result;
        m_submitted = true;

        const uint64_t generation = GpuGenerationCounter::instance().current();
        const GpuFenceIdentity preparedFence = m_fence->identity();
        std::array<GpuSurfaceCompatibility, N> compatibilities{};
        GpuSurfaceCompatibility firstCompatibility{};
        bool haveOwner = false;
        try {
            for (size_t i = 0; i < N; ++i) {
                const auto& owner = surfaces.owners()[i];
                if (!owner) continue;
                const GpuSurfaceCompatibility compatibility = owner->compatibility();
                if (!gpuSubmissionDetail::matchesSurfaceEvidence(
                        compatibility, preparedFence, generation,
                        GpuGenerationCounter::instance().current()))
                    return result;
                compatibilities[i] = compatibility;
                if (!haveOwner) firstCompatibility = compatibility;
                haveOwner = true;
            }
        } catch (...) {
            return result;
        }
        if (!haveOwner) return result;

        auto prepared =
            m_registry.prepareRetirement(surfaces.owners().data(), qsizetype(N), m_fence);
        if (!prepared) return result;

        GpuSubmitOutcome outcome = GpuSubmitOutcome::NotSubmitted;
        try {
            auto ticket = m_fence->submitExactForRetirement(
                preparedFence, firstCompatibility, generation, [&]() noexcept {
                    outcome = std::invoke(adapter);
                    if (outcome == GpuSubmitOutcome::NotSubmitted) return false;
                    prepared.markAccepted();
                    return true;
                });
            result.outcome = outcome;
            if (!prepared.accepted()) return result;

            if (ticket) {
                const uint64_t ticketValue = ticket->value();
                bool exact = true;
                for (size_t i = 0; i < N; ++i) {
                    if (surfaces.owners()[i] &&
                        !m_fence->validatesRetirement(*ticket, compatibilities[i])) {
                        exact = false;
                        break;
                    }
                }
                if (exact && m_registry.publishPrepared(prepared, std::move(*ticket))) {
                    result.retirement = GpuRetirementDisposition::Published;
                    result.fenceValue = m_fenceValue = ticketValue;
                } else {
                    m_registry.quarantinePrepared(prepared);
                    result.retirement = GpuRetirementDisposition::Quarantined;
                }
            } else {
                m_registry.quarantinePrepared(prepared);
                result.retirement = GpuRetirementDisposition::Quarantined;
            }
        } catch (...) {
            result.outcome = outcome;
            if (prepared.accepted()) {
                m_registry.quarantinePrepared(prepared);
                result.retirement = GpuRetirementDisposition::Quarantined;
            }
        }

        if (result.retirement == GpuRetirementDisposition::Quarantined) {
            m_registry.noteSignalFailure();
            try {
                GpuDeviceLossMonitor::instance().recordSubmissionFailure(
                    preparedFence.deviceDomainId);
            } catch (...) {
            }
        } else if (result.outcome == GpuSubmitOutcome::SubmittedWithError) {
            try {
                GpuDeviceLossMonitor::instance().recordSubmissionFailure(
                    preparedFence.deviceDomainId);
            } catch (...) {
            }
        }
        return result;
    }

private:
    std::shared_ptr<GpuFence> m_fence;
    GpuRetireRegistry& m_registry;
    bool m_submitted = false;
    uint64_t m_fenceValue = 0;
};

#endif // OLR_GPU_OP_SCOPE_H
