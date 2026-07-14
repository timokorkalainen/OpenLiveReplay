#ifndef OLR_TESTS_PERF_SAVED912_GPU_OP_SCOPE_H
#define OLR_TESTS_PERF_SAVED912_GPU_OP_SCOPE_H

#include "saved912preparedregistry.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpusurface.h"

#include <array>
#include <functional>
#include <memory>
#include <type_traits>

template <typename Fence>
class Saved912GpuOpScope final {
public:
    Saved912GpuOpScope(std::shared_ptr<Fence> fence, Saved912PreparedRegistry& registry)
        : m_fence(std::move(fence)), m_registry(registry) {}

    template <typename BackendAdapter, size_t N>
    GpuSubmissionResult submit(BackendAdapter& adapter, GpuSurfacePack<N> surfaces) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<GpuSubmitOutcome, BackendAdapter&>);
        GpuSubmissionResult result;
        if (m_submitted || !m_fence) return result;
        m_submitted = true;
        auto allocationPhase = m_registry.beginAllocationScope();
        const uint64_t generation = GpuGenerationCounter::instance().current();
        const GpuFenceIdentity preparedFence = m_fence->identity();
        std::array<GpuSurfaceCompatibility, N> compatibilities{};
        GpuSurfaceCompatibility firstCompatibility{};
        try {
            for (size_t i = 0; i < N; ++i) {
                const auto& owner = surfaces.owners()[i];
                if (!owner) return result;
                for (size_t previous = 0; previous < i; ++previous) {
                    if (surfaces.owners()[previous].get() == owner.get()) return result;
                }
            }
            for (size_t i = 0; i < N; ++i) {
                const auto& owner = surfaces.owners()[i];
                const auto compatibility = owner->compatibility();
                if (!gpuSubmissionDetail::matchesSurfaceEvidence(
                        compatibility, preparedFence, generation,
                        GpuGenerationCounter::instance().current()))
                    return result;
                compatibilities[i] = compatibility;
                if (i == 0) firstCompatibility = compatibility;
            }
        } catch (...) {
            return result;
        }
        auto prepared =
            m_registry.prepareRetirement(surfaces.owners().data(), qsizetype(N), m_fence);
        if (!prepared) return result;
        GpuSubmitOutcome outcome = GpuSubmitOutcome::NotSubmitted;
        try {
            auto ticket = m_fence->submitForBaseline(
                preparedFence, firstCompatibility, generation, [&]() noexcept {
                    allocationPhase.enterCallback();
                    outcome = std::invoke(adapter);
                    if (outcome == GpuSubmitOutcome::NotSubmitted) return false;
                    prepared.markAccepted();
                    allocationPhase.enterPostAccept();
                    return true;
                });
            result.outcome = outcome;
            if (!prepared.accepted()) return result;
            if (ticket) {
                const uint64_t value = ticket->value();
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
                    result.fenceValue = value;
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
        if (result.retirement == GpuRetirementDisposition::Quarantined)
            m_registry.noteSignalFailure();
        return result;
    }

private:
    std::shared_ptr<Fence> m_fence;
    Saved912PreparedRegistry& m_registry;
    bool m_submitted = false;
};

#endif
