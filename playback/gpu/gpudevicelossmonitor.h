#ifndef OLR_GPUDEVICELOSSMONITOR_H
#define OLR_GPUDEVICELOSSMONITOR_H

#include "playback/gpu/gpusurfacelease.h" // DeadDeviceToken
#include "playback/gpu/gpurecoverycoordinator.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>
#include <functional>
#include <limits>

class GpuRetireRegistry;
#ifdef OLR_UNIT_TEST
struct GpuRetireRegistryTestAuthority;
#endif

class GpuValidatedDeadDomains final {
public:
    GpuValidatedDeadDomains(const GpuValidatedDeadDomains&) = delete;
    GpuValidatedDeadDomains& operator=(const GpuValidatedDeadDomains&) = delete;
    bool hasAuthoritativeProof() const noexcept { return !m_tokens.empty(); }
    bool authorizes(uintptr_t deviceDomainId, uint64_t authorityEpoch) const noexcept {
        for (const DeadDeviceToken& token : m_tokens) {
            if (token.deviceDomainId() == deviceDomainId &&
                token.authorityEpoch() == authorityEpoch)
                return true;
        }
        return false;
    }
    template <typename Fn>
    void forEachEvidence(Fn&& fn) const {
        for (const DeadDeviceToken& token : m_tokens)
            std::invoke(std::forward<Fn>(fn), token.deviceDomainId(), token.authorityEpoch());
    }

private:
    friend class GpuDeviceLossMonitor;
    friend class GpuRetireRegistry;
#ifdef OLR_UNIT_TEST
    friend struct GpuRetireRegistryTestAuthority;
#endif
    explicit GpuValidatedDeadDomains(const std::vector<DeadDeviceToken>& tokens)
        : m_tokens(tokens) {}
    const std::vector<DeadDeviceToken>& m_tokens;
};

class GpuRhiContext;
class WinGpuImportEdge;
#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority;
#endif

// Process-wide GPU device-loss latch. A loss is a hard-down for a live tool:
// recordLoss() bumps GpuGenerationCounter so every FrameHandle stamped under
// the dead device is stale, and consumeLossEvent() drains telemetry events.
class GpuDeviceLossMonitor {
public:
    static GpuDeviceLossMonitor& instance();

    bool isLost() const;
    bool isCurrentDeviceAuthority(uint64_t authorityEpoch) const noexcept {
        return authorityEpoch != 0 &&
               m_publishedDeviceAuthorityEpoch.load(std::memory_order_acquire) == authorityEpoch;
    }
    uint64_t lossCount() const;

    // Idempotent while the latch is already lost. A fresh loss epoch begins only
    // after clearForRebuild() has cleared the latch following a successful rebuild.
    uint64_t recordLoss();
    uint64_t recordSubmissionFailure(uintptr_t deviceDomainId);

    // Carries the DeadDeviceToken from the driver-authoritative detection site to
    // the worker's recovery path (handleGpuDeviceLoss), which reads realLossToken()
    // to decide between the token-gated no-wait free (REAL loss) and the bounded-
    // wait drain (INJECTED loss, no token). A tokenless submission-failure epoch may
    // be upgraded by later driver-authoritative proof from the same device generation.
    // The injected-loss path calls only recordLoss(), so without that proof no token
    // is stored and the no-wait free can never run on a live device. A delayed/stale
    // mark is rejected unless its creation-time authority epoch still matches the
    // current rebuild epoch.
    std::optional<DeadDeviceToken> realLossToken() const;
    std::vector<DeadDeviceToken> realLossTokens() const;

    template <typename Fn>
    GpuValidatedLossResult withValidatedDeadDomains(Fn&& fn) {
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        if (generation == 0 || !m_lost.load(std::memory_order_acquire) || m_rebuildInProgress)
            return {};
        if (m_realLossTokens.empty()) return {};
        for (const DeadDeviceToken& token : m_realLossTokens) {
            if (token.observedGeneration() != generation ||
                token.authorityEpoch() != m_deviceAuthorityEpoch)
                return {};
        }
        return GpuRecoveryCoordinator::instance().coordinate(generation, m_realLossRevision, [&]() {
            GpuValidatedDeadDomains domains(m_realLossTokens);
            return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                          std::invoke(std::forward<Fn>(fn), domains)};
        });
    }

    template <typename Fn>
    GpuValidatedLossResult withCoordinatedTokenlessRecovery(Fn&& fn) {
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        if (generation == 0 || !m_lost.load(std::memory_order_acquire) || m_rebuildInProgress ||
            !m_realLossTokens.empty())
            return {};
        return GpuRecoveryCoordinator::instance().coordinate(
            generation, std::numeric_limits<uint64_t>::max(), [&]() {
                return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                              std::invoke(std::forward<Fn>(fn))};
            });
    }

    bool consumeLossEvent();
    // Invalidate authorities owned by the old device before constructing its
    // replacement. The loss latch remains set until clearForRebuild() commits a
    // successful rebuild.
    void beginRebuild();
    void clearForRebuild();
    void reset();
#ifdef OLR_UNIT_TEST
    uint64_t currentDeviceAuthorityForTest() const noexcept {
        return m_publishedDeviceAuthorityEpoch.load(std::memory_order_acquire);
    }
#endif

private:
    GpuDeviceLossMonitor() = default;
    friend class GpuRhiContext;
    friend class WinGpuImportEdge;
#ifdef OLR_UNIT_TEST
    friend struct GpuDeviceLossMonitorTestAuthority;
#endif

    uint64_t captureDeviceAuthorityEpoch() const;
    uint64_t publishRealDeviceLoss(DeadDeviceToken::Provenance provenance,
                                   uint64_t deviceAuthorityEpoch, uintptr_t deviceDomainId);

    std::atomic<bool> m_lost{false};
    std::atomic<uint64_t> m_lossCount{0};
    std::atomic<uint64_t> m_undrained{0};
    std::atomic<uint64_t> m_lossGeneration{0};
    std::atomic<uint64_t> m_publishedDeviceAuthorityEpoch{1};
    mutable std::mutex m_epochMutex;
    uint64_t m_deviceAuthorityEpoch = 1;            // guarded by m_epochMutex
    bool m_rebuildInProgress = false;               // guarded by m_epochMutex
    std::optional<DeadDeviceToken> m_realLossToken; // guarded by m_epochMutex
    std::vector<DeadDeviceToken> m_realLossTokens;  // guarded by m_epochMutex
    uint64_t m_realLossRevision = 0;                // guarded by m_epochMutex
};

#endif // OLR_GPUDEVICELOSSMONITOR_H
