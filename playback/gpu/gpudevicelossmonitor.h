#ifndef OLR_GPUDEVICELOSSMONITOR_H
#define OLR_GPUDEVICELOSSMONITOR_H

#include "playback/gpu/gpusurfacelease.h" // DeadDeviceToken
#include "playback/gpu/gpurecoverycoordinator.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <limits>
#include <memory>

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
            std::invoke(fn, token.deviceDomainId(), token.authorityEpoch());
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
class QSemaphore;
#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority;
#endif

class GpuRecoveryTicket final {
public:
    bool isValid() const noexcept { return m_lossGeneration != 0 && m_participantId != 0; }
    uint64_t lossGeneration() const noexcept { return m_lossGeneration; }
    uint64_t authorityEpoch() const noexcept { return m_authorityEpoch; }

private:
    friend class GpuDeviceLossMonitor;
    GpuRecoveryTicket(uint64_t lossGeneration, uint64_t proofRevision, uint64_t authorityEpoch,
                      uint64_t recoveryRevision, uint64_t participantId) noexcept
        : m_lossGeneration(lossGeneration), m_proofRevision(proofRevision),
          m_authorityEpoch(authorityEpoch), m_recoveryRevision(recoveryRevision),
          m_participantId(participantId) {}

    uint64_t m_lossGeneration = 0;
    uint64_t m_proofRevision = 0;
    uint64_t m_authorityEpoch = 0;
    uint64_t m_recoveryRevision = 0;
    uint64_t m_participantId = 0;
};

class GpuRecoveryRegistration final {
public:
    bool isValid() const noexcept { return m_participantId != 0; }
    uint64_t participantId() const noexcept { return m_participantId; }
    uint64_t lossGeneration() const noexcept { return m_lossGeneration; }

private:
    friend class GpuDeviceLossMonitor;
    GpuRecoveryRegistration(uint64_t participantId, uint64_t lossGeneration) noexcept
        : m_participantId(participantId), m_lossGeneration(lossGeneration) {}

    uint64_t m_participantId = 0;
    uint64_t m_lossGeneration = 0;
};

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
    uint64_t currentDeviceAuthorityEpoch() const noexcept {
        return m_publishedDeviceAuthorityEpoch.load(std::memory_order_acquire);
    }
    uint64_t currentLossGeneration() const noexcept {
        return m_lossGeneration.load(std::memory_order_acquire);
    }
    uint64_t lossCount() const;
    bool authorizesCurrentDeadDomain(GpuSurfaceCompatibility compatibility) const noexcept;
    bool authorizesTerminalDeadDomain(uint64_t participantId,
                                      GpuSurfaceCompatibility compatibility) const noexcept;

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
    GpuValidatedLossResult withValidatedDeadDomains(Fn&& fn) noexcept {
        try {
#ifdef OLR_UNIT_TEST
            noteRecoveryAttemptForTest();
#endif
            std::unique_lock<std::mutex> deliveryLock(m_proofDeliveryMutex);
            std::unique_lock<std::mutex> epochLock(m_epochMutex);
            const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
            if (generation == 0 || !m_lost.load(std::memory_order_acquire) || m_rebuildInProgress)
                return {};
            const std::shared_ptr<const std::vector<DeadDeviceToken>> proof = m_realLossProof;
            if (!proof || proof->empty()) {
                m_tokenlessRecoveryObserved = true;
                return {};
            }
            for (const DeadDeviceToken& token : *proof) {
                if (token.observedGeneration() != generation ||
                    token.authorityEpoch() != m_deviceAuthorityEpoch)
                    return {};
            }
            const uint64_t revision = m_realLossRevision;
            epochLock.unlock();
            const GpuValidatedLossResult result =
                GpuRecoveryCoordinator::instance().coordinate(generation, revision, [&]() {
                    GpuValidatedDeadDomains domains(*proof);
                    return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                                  std::invoke(std::forward<Fn>(fn), domains)};
                });
            if (result.status == GpuValidatedLossStatus::Completed) {
                epochLock.lock();
                if (m_lossGeneration.load(std::memory_order_acquire) == generation &&
                    m_realLossRevision == revision)
                    m_deliveredProofRevision = revision;
            }
            return result;
        } catch (...) {
            return {};
        }
    }

    template <typename Fn>
    GpuValidatedLossResult withCoordinatedTokenlessRecovery(Fn&& fn) noexcept {
        try {
#ifdef OLR_UNIT_TEST
            noteRecoveryAttemptForTest();
#endif
            std::unique_lock<std::mutex> deliveryLock(m_proofDeliveryMutex);
            std::unique_lock<std::mutex> epochLock(m_epochMutex);
            const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
            if (generation == 0 || !m_lost.load(std::memory_order_acquire) || m_rebuildInProgress ||
                (m_realLossProof && !m_realLossProof->empty()))
                return {};
            m_tokenlessRecoveryObserved = true;
            epochLock.unlock();
            return GpuRecoveryCoordinator::instance().coordinate(
                generation, std::numeric_limits<uint64_t>::max(), [&]() {
                    return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                                  std::invoke(std::forward<Fn>(fn))};
                });
        } catch (...) {
            return {};
        }
    }

    bool consumeLossEvent();
    // Output graphs register for process-wide recovery coordination. A loss snapshots
    // the then-live participants; replacement authority is minted once, and the latch
    // clears only after every snapshotted participant has acknowledged teardown/rebuild.
    uint64_t registerRecoveryParticipant(bool ownsCurrentDevice = true);
    GpuRecoveryRegistration registerRecoveryParticipantSnapshot(bool ownsCurrentDevice = true);
    uint64_t registerTerminalRecoveryParticipant();
    // Terminal-quarantine boundary only. Converts an existing graph participant
    // into a persistent observer before its worker ownership is transferred.
    bool promoteRecoveryParticipantToTerminal(uint64_t participantId) noexcept;
    // nullopt means active-epoch cleanup was rejected and the participant remains registered.
    std::optional<qsizetype> unregisterRecoveryParticipant(uint64_t participantId) noexcept;
    // Destructor-only fallback after the participant's graph and worker are
    // quiescent. Performs serialized cleanup directly, without coordinator
    // admission allocation, so a terminal owner cannot be stranded.
    bool unregisterTerminalRecoveryParticipant(uint64_t participantId) noexcept;
    bool acknowledgeRecoveryCleanup(uint64_t participantId, uint64_t lossGeneration);
    GpuRecoveryTicket beginRebuild(uint64_t participantId);
    bool clearForRebuild(const GpuRecoveryTicket& ticket);
    // Invalidate authorities owned by the old device before constructing its
    // replacement. The loss latch remains set until clearForRebuild() commits a
    // successful rebuild.
    void beginRebuild();
    void clearForRebuild();
    void reset();
#ifdef OLR_UNIT_TEST
    uint64_t currentDeviceAuthorityForTest() const noexcept {
        return currentDeviceAuthorityEpoch();
    }
    uint64_t currentLossGenerationForTest() const noexcept {
        return m_lossGeneration.load(std::memory_order_acquire);
    }
    void failNextTerminalUnregisterForTest() noexcept {
        m_rejectNextTerminalUnregisterForTest.store(true, std::memory_order_release);
    }
    void failNextTerminalProofMergeForTest() noexcept {
        m_failNextTerminalProofMergeForTest.store(true, std::memory_order_release);
    }
    void failNextTerminalPromotionForTest() noexcept {
        m_failNextTerminalPromotionForTest.store(true, std::memory_order_release);
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
    uint64_t recordTokenlessLoss();
    uint64_t publishRealDeviceLoss(DeadDeviceToken::Provenance provenance,
                                   uint64_t deviceAuthorityEpoch, uintptr_t deviceDomainId);
    void
    beginLossEpochLocked(uint64_t generation, std::unordered_set<uint64_t>&& pendingParticipants,
                         std::unordered_set<uint64_t>&& cleanupAcknowledgedParticipants) noexcept;
    void beginRecoveryLocked(uint64_t generation);
    bool snapshotTerminalProofAndRemovePendingLocked();
    uint64_t clearLossEpochLocked();
#ifdef OLR_UNIT_TEST
    void noteRecoveryAttemptForTest();
#endif

    std::atomic<bool> m_lost{false};
    std::atomic<uint64_t> m_lossCount{0};
    std::atomic<uint64_t> m_undrained{0};
    std::atomic<uint64_t> m_lossGeneration{0};
    std::atomic<uint64_t> m_publishedDeviceAuthorityEpoch{1};
    // Serializes cold-path proof publication and worker recovery through post-epoch delivery.
    std::mutex m_proofDeliveryMutex;
    mutable std::mutex m_epochMutex;
    uint64_t m_deviceAuthorityEpoch = 1;            // guarded by m_epochMutex
    bool m_rebuildInProgress = false;               // guarded by m_epochMutex
    std::optional<DeadDeviceToken> m_realLossToken; // guarded by m_epochMutex
    // Immutable snapshots let recovery carry exact proof beyond m_epochMutex without
    // allocating during teardown. A new revision is built before it is published.
    std::shared_ptr<const std::vector<DeadDeviceToken>> m_realLossProof; // guarded by m_epochMutex
    uint64_t m_realLossRevision = 0;                                     // guarded by m_epochMutex
    uint64_t m_deliveredProofRevision = 0;                               // guarded by m_epochMutex
    bool m_tokenlessRecoveryObserved = false;                            // guarded by m_epochMutex
    uint64_t m_nextRecoveryParticipantId = 1;                            // guarded by m_epochMutex
    std::unordered_set<uint64_t> m_recoveryParticipants;                 // guarded by m_epochMutex
    std::unordered_set<uint64_t> m_terminalRecoveryParticipants;         // guarded by m_epochMutex
    std::unordered_map<uint64_t, std::vector<DeadDeviceToken>>
        m_terminalDeadDomainProofs;                                 // guarded by m_epochMutex
    std::unordered_set<uint64_t> m_pendingRecoveryParticipants;     // guarded by m_epochMutex
    std::unordered_set<uint64_t> m_cleanupAcknowledgedParticipants; // guarded by m_epochMutex
    uint64_t m_recoveryGeneration = 0;                              // guarded by m_epochMutex
    uint64_t m_recoveryRevision = 0;                                // guarded by m_epochMutex
    uint64_t m_recoveryAuthorityEpoch = 0;                          // guarded by m_epochMutex
    uint64_t m_cleanupAuthorityEpoch = 0;                           // guarded by m_epochMutex
    uint64_t m_cleanupGeneration = 0;                               // guarded by m_epochMutex
#ifdef OLR_UNIT_TEST
    QSemaphore* m_proofAcceptedForTest = nullptr;
    QSemaphore* m_continueProofDeliveryForTest = nullptr;
    QSemaphore* m_recoveryAttemptingForTest = nullptr;
    QSemaphore* m_beforeNoLossUnregisterEraseForTest = nullptr;
    QSemaphore* m_continueNoLossUnregisterEraseForTest = nullptr;
    QSemaphore* m_afterDeliveryLockForTest = nullptr;
    QSemaphore* m_continueAfterDeliveryLockForTest = nullptr;
    QSemaphore* m_epochLockHeldForTest = nullptr;
    QSemaphore* m_continueEpochLockForTest = nullptr;
    bool m_failNextLossPreparationForTest = false;
    bool m_failRegistrationAfterPrimaryInsertForTest = false;
    std::atomic<bool> m_rejectNextTerminalUnregisterForTest{false};
    std::atomic<bool> m_failNextTerminalProofMergeForTest{false};
    std::atomic<bool> m_failNextTerminalPromotionForTest{false};
    size_t m_lastImmediateProofSizeForTest = 0;
#endif
};

#endif // OLR_GPUDEVICELOSSMONITOR_H
