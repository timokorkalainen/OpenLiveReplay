#ifndef OLR_GPU_OP_SCOPE_H
#define OLR_GPU_OP_SCOPE_H

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/gpu/gpusubmission.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    // Driver acceptance is irreversible even if signalling later quarantines retirement.
    bool submitted() const noexcept { return m_driverAccepted; }
    // Cancellation is only meaningful before the one permitted submission attempt.
    bool cancel() noexcept {
        if (m_state != State::Ready) return false;
        m_state = State::Cancelled;
        return true;
    }

    template <typename BackendAdapter, size_t N>
    GpuSubmissionResult submit(BackendAdapter& adapter, GpuSurfacePack<N> surfaces) noexcept {
        using ScopedView = GpuScopedNativeView<N>;
        static_assert(
            std::is_nothrow_invocable_r_v<GpuSubmitOutcome, BackendAdapter&, const ScopedView&>,
            "GPU backend adapters must be noexcept and accept the exact GpuScopedNativeView");
        return submitImpl<true>(adapter, std::move(surfaces));
    }

    // Capability-free submission path for operations that only need exact retained
    // ownership. The callback receives no surface/view, so it cannot touch native data.
    template <typename BackendAdapter, size_t N>
    GpuSubmissionResult submitRetained(BackendAdapter& adapter,
                                       GpuSurfacePack<N> surfaces) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<GpuSubmitOutcome, BackendAdapter&>,
                      "retained-only GPU adapters must be noexcept and accept no arguments");
        return submitImpl<false>(adapter, std::move(surfaces));
    }

private:
    template <bool ExposeNative, typename BackendAdapter, size_t N>
    GpuSubmissionResult submitImpl(BackendAdapter& adapter, GpuSurfacePack<N> surfaces) noexcept {
        static_assert(N <= 17, "A fused GPU submission supports at most 17 surface owners");
        using ScopedView = GpuScopedNativeView<N>;

        GpuSubmissionResult result;
        if (m_state != State::Ready || !m_fence || GpuDeviceLossMonitor::instance().isLost())
            return result;
        m_state = State::Consumed;
        auto allocationPhase = m_registry.beginAllocationScope();

        const uint64_t generation = GpuGenerationCounter::instance().current();
        const GpuFenceIdentity preparedFence = m_fence->identity();
        if (!GpuDeviceLossMonitor::instance().isCurrentDeviceAuthority(
                preparedFence.authorityEpoch))
            return result;
        std::array<GpuSurfaceCompatibility, N> compatibilities{};
        std::array<size_t, N> uniqueIndices{};
        std::optional<std::array<std::shared_ptr<GpuSurface>, N>> coalescedOwners;
        size_t uniqueCount = 0;
        GpuSurfaceCompatibility firstCompatibility{};
        try {
            for (size_t i = 0; i < N; ++i) {
                const auto& owner = surfaces.m_surfaces[i];
                if (!owner) return result;
                bool duplicate = false;
                for (size_t previous = 0; previous < uniqueCount; ++previous) {
                    if (surfaces.m_surfaces[uniqueIndices[previous]].get() == owner.get()) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;
                const GpuSurfaceCompatibility compatibility = owner->compatibility();
                if (!gpuSubmissionDetail::matchesSurfaceEvidence(
                        compatibility, preparedFence, generation,
                        GpuGenerationCounter::instance().current()))
                    return result;
                uniqueIndices[uniqueCount] = i;
                compatibilities[uniqueCount] = compatibility;
                if (uniqueCount == 0) firstCompatibility = compatibility;
                ++uniqueCount;
            }
        } catch (...) {
            return result;
        }
        const std::shared_ptr<GpuSurface>* retirementOwners = surfaces.m_surfaces.data();
        if (uniqueCount != N) {
            coalescedOwners.emplace();
            for (size_t i = 0; i < uniqueCount; ++i)
                (*coalescedOwners)[i] = surfaces.m_surfaces[uniqueIndices[i]];
            retirementOwners = coalescedOwners->data();
        }
        auto prepared =
            m_registry.prepareRetirement(retirementOwners, qsizetype(uniqueCount), m_fence);
        if (!prepared) return result;

        GpuSubmitOutcome outcome = GpuSubmitOutcome::NotSubmitted;
        try {
            auto submitScoped = [&](const ScopedView* view) {
                auto ticket = m_fence->submitExactForRetirement(
                    preparedFence, firstCompatibility, generation, [&]() noexcept {
                        allocationPhase.enterCallback();
                        if constexpr (ExposeNative)
                            outcome = std::invoke(adapter, *view);
                        else
                            outcome = std::invoke(adapter);
                        if (outcome == GpuSubmitOutcome::NotSubmitted) return false;
                        m_driverAccepted = true;
                        prepared.markAccepted();
                        allocationPhase.enterPostAccept();
                        return true;
                    });
                result.outcome = outcome;
                if (!prepared.accepted()) return;

                if (ticket) {
                    const uint64_t ticketValue = ticket->value();
                    bool exact = true;
                    for (size_t i = 0; i < uniqueCount; ++i) {
                        if (!m_fence->validatesRetirement(*ticket, compatibilities[i])) {
                            exact = false;
                            break;
                        }
                    }
                    if (exact && m_registry.publishPrepared(prepared, std::move(*ticket))) {
                        for (size_t i = 0; i < uniqueCount; ++i) {
                            retirementOwners[i]->retainUntilFenceRetired(ticketValue);
#ifdef OLR_UNIT_TEST
                            ++s_stampCountForTest;
#endif
                        }
                        result.retirement = GpuRetirementDisposition::Published;
                        result.producerFence = std::move(m_fence);
                        result.fenceValue = m_fenceValue = ticketValue;
                    } else {
                        m_registry.quarantinePrepared(prepared);
                        result.retirement = GpuRetirementDisposition::Quarantined;
                    }
                } else {
                    m_registry.quarantinePrepared(prepared);
                    result.retirement = GpuRetirementDisposition::Quarantined;
                }
            };
            if constexpr (ExposeNative) {
                auto invokeScoped = [&](const ScopedView& view) { submitScoped(&view); };
                withScopedNativeView(surfaces, invokeScoped);
            } else {
                submitScoped(nullptr);
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

#ifdef OLR_UNIT_TEST
public:
    static void resetStampCountForTest() noexcept { s_stampCountForTest = 0; }
    static size_t stampCountForTest() noexcept { return s_stampCountForTest; }

private:
    inline static thread_local size_t s_stampCountForTest = 0;
#endif
    template <size_t N, typename Fn>
    static void withScopedNativeView(GpuSurfacePack<N>& surfaces, Fn& fn) {
        GpuSyncReadScope::withRetainedBatch([&](GpuSyncReadState& state) {
            const GpuScopedNativeView<N> view(surfaces.m_surfaces, &state);
            std::invoke(fn, view);
        });
    }

    enum class State : uint8_t { Ready, Cancelled, Consumed };

    std::shared_ptr<GpuFence> m_fence;
    GpuRetireRegistry& m_registry;
    State m_state = State::Ready;
    bool m_driverAccepted = false;
    uint64_t m_fenceValue = 0;
};

#endif // OLR_GPU_OP_SCOPE_H
