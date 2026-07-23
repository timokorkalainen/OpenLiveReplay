#include "playback/gpu/gpudevicelossmonitor.h"

#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpuretireregistry.h"

#ifdef OLR_UNIT_TEST
#include <QSemaphore>
#endif

GpuDeviceLossMonitor& GpuDeviceLossMonitor::instance() {
    static GpuDeviceLossMonitor monitor;
    return monitor;
}

#ifdef OLR_UNIT_TEST
void GpuDeviceLossMonitor::noteRecoveryAttemptForTest() {
    if (m_recoveryAttemptingForTest) m_recoveryAttemptingForTest->release();
}
#endif

bool GpuDeviceLossMonitor::isLost() const {
    return m_lost.load(std::memory_order_acquire);
}

uint64_t GpuDeviceLossMonitor::lossCount() const {
    return m_lossCount.load(std::memory_order_acquire);
}

bool GpuDeviceLossMonitor::authorizesCurrentDeadDomain(
    GpuSurfaceCompatibility compatibility) const noexcept {
    try {
        if (compatibility.deviceDomainId == 0 || compatibility.authorityEpoch == 0) return false;
        std::lock_guard<std::mutex> lock(m_epochMutex);
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        if (generation == 0 || !m_lost.load(std::memory_order_acquire) || !m_realLossProof)
            return false;
        for (const DeadDeviceToken& token : *m_realLossProof) {
            if (token.observedGeneration() == generation &&
                token.deviceDomainId() == compatibility.deviceDomainId &&
                token.authorityEpoch() == compatibility.authorityEpoch) {
                return true;
            }
        }
    } catch (...) {
        // A proof query is conservative when the monitor cannot be inspected.
        static_cast<void>(0);
    }
    return false;
}

bool GpuDeviceLossMonitor::authorizesTerminalDeadDomain(
    uint64_t participantId, GpuSurfaceCompatibility compatibility) const noexcept {
    try {
        if (participantId == 0 || compatibility.deviceDomainId == 0 ||
            compatibility.authorityEpoch == 0)
            return false;
        std::lock_guard<std::mutex> lock(m_epochMutex);
        if (m_terminalRecoveryParticipants.count(participantId) == 0) return false;
        const auto retained = m_terminalDeadDomainProofs.find(participantId);
        if (retained != m_terminalDeadDomainProofs.end()) {
            for (const DeadDeviceToken& token : retained->second) {
                if (token.deviceDomainId() == compatibility.deviceDomainId &&
                    token.authorityEpoch() == compatibility.authorityEpoch)
                    return true;
            }
        }
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        if (generation == 0 || !m_lost.load(std::memory_order_acquire) || !m_realLossProof)
            return false;
        for (const DeadDeviceToken& token : *m_realLossProof) {
            if (token.observedGeneration() == generation &&
                token.deviceDomainId() == compatibility.deviceDomainId &&
                token.authorityEpoch() == compatibility.authorityEpoch)
                return true;
        }
    } catch (...) {
        // A terminal proof query is conservative when state cannot be inspected.
        static_cast<void>(0);
    }
    return false;
}

void GpuDeviceLossMonitor::beginLossEpochLocked(
    uint64_t generation, std::unordered_set<uint64_t>&& pendingParticipants,
    std::unordered_set<uint64_t>&& cleanupAcknowledgedParticipants) noexcept {
    m_rebuildInProgress = false;
    m_realLossToken.reset();
    m_realLossProof.reset();
    m_realLossRevision = 0;
    m_deliveredProofRevision = 0;
    m_tokenlessRecoveryObserved = false;
    m_pendingRecoveryParticipants = std::move(pendingParticipants);
    m_cleanupAcknowledgedParticipants = std::move(cleanupAcknowledgedParticipants);
    m_recoveryGeneration = 0;
    m_recoveryAuthorityEpoch = 0;
    if (++m_recoveryRevision == 0) ++m_recoveryRevision;
    m_lossGeneration.store(generation, std::memory_order_release);
}

void GpuDeviceLossMonitor::beginRecoveryLocked(uint64_t generation) {
    if (m_recoveryGeneration == generation) return;
    m_cleanupAuthorityEpoch = m_deviceAuthorityEpoch;
    m_cleanupGeneration = generation;
    if (++m_deviceAuthorityEpoch == 0) ++m_deviceAuthorityEpoch;
    m_publishedDeviceAuthorityEpoch.store(m_deviceAuthorityEpoch, std::memory_order_release);
    m_rebuildInProgress = true;
    m_recoveryGeneration = generation;
    m_recoveryAuthorityEpoch = m_deviceAuthorityEpoch;
}

uint64_t GpuDeviceLossMonitor::clearLossEpochLocked() {
    const uint64_t retiredGeneration = m_lossGeneration.load(std::memory_order_acquire);
    m_rebuildInProgress = false;
    m_lost.store(false, std::memory_order_release);
    m_lossGeneration.store(0, std::memory_order_release);
    m_realLossToken.reset();
    m_realLossProof.reset();
    m_realLossRevision = 0;
    m_deliveredProofRevision = 0;
    m_tokenlessRecoveryObserved = false;
    m_pendingRecoveryParticipants.clear();
    m_cleanupAcknowledgedParticipants.clear();
    m_recoveryGeneration = 0;
    m_recoveryAuthorityEpoch = 0;
    return retiredGeneration;
}

bool GpuDeviceLossMonitor::snapshotTerminalProofAndRemovePendingLocked() {
    try {
        auto nextProofs = m_terminalDeadDomainProofs;
        if (m_realLossProof) {
            for (const uint64_t participantId : m_terminalRecoveryParticipants) {
                auto& retained = nextProofs[participantId];
                for (const DeadDeviceToken& token : *m_realLossProof) {
                    bool duplicate = false;
                    for (const DeadDeviceToken& existing : retained) {
                        if (existing.observedGeneration() == token.observedGeneration() &&
                            existing.deviceDomainId() == token.deviceDomainId() &&
                            existing.authorityEpoch() == token.authorityEpoch()) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) retained.push_back(token);
                }
            }
        }
        m_terminalDeadDomainProofs.swap(nextProofs);
        for (const uint64_t participantId : m_terminalRecoveryParticipants) {
            m_pendingRecoveryParticipants.erase(participantId);
            m_cleanupAcknowledgedParticipants.erase(participantId);
        }
        return true;
    } catch (...) {
        // Do not mutate proof or participant state unless the full snapshot exists.
        return false;
    }
}

uint64_t GpuDeviceLossMonitor::recordTokenlessLoss() {
    constexpr int kTokenlessDrainMs = 100;
    uint64_t generation = 0;
    uint64_t retiredGeneration = 0;
    {
        std::unique_lock<std::mutex> deliveryLock(m_proofDeliveryMutex);
#ifdef OLR_UNIT_TEST
        if (m_afterDeliveryLockForTest) {
            m_afterDeliveryLockForTest->release();
            if (m_continueAfterDeliveryLockForTest) m_continueAfterDeliveryLockForTest->acquire();
        }
#endif
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        if (m_lost.load(std::memory_order_acquire)) {
            generation = m_lossGeneration.load(std::memory_order_acquire);
            // With no participant, publication itself owns cleanup. A rejected
            // attempt must remain retryable on the next identical observation.
            if (m_rebuildInProgress || !m_pendingRecoveryParticipants.empty() ||
                (m_realLossProof && !m_realLossProof->empty())) {
                return generation;
            }
            m_tokenlessRecoveryObserved = true;
        } else {
            auto pendingParticipants = m_recoveryParticipants;
            auto cleanupAcknowledgedParticipants = m_terminalRecoveryParticipants;
#ifdef OLR_UNIT_TEST
            if (m_failNextLossPreparationForTest) {
                m_failNextLossPreparationForTest = false;
                throw std::bad_alloc();
            }
#endif
            generation = GpuGenerationCounter::instance().bump();
            beginLossEpochLocked(generation, std::move(pendingParticipants),
                                 std::move(cleanupAcknowledgedParticipants));
            m_lossCount.fetch_add(1, std::memory_order_acq_rel);
            m_undrained.fetch_add(1, std::memory_order_acq_rel);
            m_lost.store(true, std::memory_order_release);
        }
        if (m_pendingRecoveryParticipants.empty()) {
            m_tokenlessRecoveryObserved = true;
            epochLock.unlock();
            GpuRetireRegistry registry;
            const GpuValidatedLossResult cleanup = GpuRecoveryCoordinator::instance().coordinate(
                generation, std::numeric_limits<uint64_t>::max(), [&]() {
                    registry.drainWithBoundedWait(kTokenlessDrainMs);
                    return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, 0};
                });
            epochLock.lock();
            if (cleanup.status == GpuValidatedLossStatus::Completed &&
                m_lost.load(std::memory_order_acquire) &&
                m_lossGeneration.load(std::memory_order_acquire) == generation &&
                m_pendingRecoveryParticipants.empty()) {
                beginRecoveryLocked(generation);
                retiredGeneration = clearLossEpochLocked();
            }
        }
    }
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
    return generation;
}

uint64_t GpuDeviceLossMonitor::recordLoss() {
    return recordTokenlessLoss();
}

uint64_t GpuDeviceLossMonitor::captureDeviceAuthorityEpoch() const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
#ifdef OLR_UNIT_TEST
    if (m_epochLockHeldForTest) {
        m_epochLockHeldForTest->release();
        if (m_continueEpochLockForTest) m_continueEpochLockForTest->acquire();
    }
#endif
    return m_deviceAuthorityEpoch;
}

uint64_t GpuDeviceLossMonitor::publishRealDeviceLoss(DeadDeviceToken::Provenance provenance,
                                                     uint64_t deviceAuthorityEpoch,
                                                     uintptr_t deviceDomainId) {
    std::unique_lock<std::mutex> deliveryLock(m_proofDeliveryMutex);
#ifdef OLR_UNIT_TEST
    if (m_afterDeliveryLockForTest) {
        m_afterDeliveryLockForTest->release();
        if (m_continueAfterDeliveryLockForTest) m_continueAfterDeliveryLockForTest->acquire();
    }
#endif
    uint64_t generation = 0;
    uint64_t supersededGeneration = 0;
    uint64_t acceptedRevision = 0;
    std::vector<DeadDeviceToken> acceptedProof;
    std::shared_ptr<const std::vector<DeadDeviceToken>> deliveryProof;
    bool immediateDeliveryRequired = false;
    bool cleanupOnlyProof = false;
    bool clearZeroParticipantEpoch = false;
    {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        if (deviceAuthorityEpoch != m_deviceAuthorityEpoch) {
            generation = deviceAuthorityEpoch == m_cleanupAuthorityEpoch && m_cleanupGeneration != 0
                             ? m_cleanupGeneration
                             : GpuGenerationCounter::instance().current();
            try {
                acceptedProof.push_back(
                    DeadDeviceToken(provenance, generation, deviceDomainId, deviceAuthorityEpoch));
            } catch (...) {
                return 0;
            }
            cleanupOnlyProof = true;
        }
        const bool replacementDeviceLoss =
            m_lost.load(std::memory_order_acquire) && m_rebuildInProgress &&
            m_recoveryGeneration == m_lossGeneration.load(std::memory_order_acquire) &&
            deviceAuthorityEpoch == m_recoveryAuthorityEpoch;
        if (cleanupOnlyProof) {
            // A participant may publish one last exact-domain proof after replacement
            // authority was minted. It can clean matching old-authority retirements,
            // but cannot mutate the current loss epoch.
        } else if (m_lost.load(std::memory_order_acquire) && !replacementDeviceLoss) {
            generation = m_lossGeneration.load(std::memory_order_acquire);
            const auto currentProof = m_realLossProof;
            bool duplicateDomain = false;
            if (currentProof) {
                for (const DeadDeviceToken& token : *currentProof) {
                    if (token.deviceDomainId() == deviceDomainId) {
                        duplicateDomain = true;
                        break;
                    }
                }
            }
            if (duplicateDomain) {
                const bool publicationOwnsRetry =
                    m_tokenlessRecoveryObserved || m_pendingRecoveryParticipants.empty();
                if (!publicationOwnsRetry || m_deliveredProofRevision >= m_realLossRevision)
                    return generation;
                immediateDeliveryRequired = true;
                acceptedRevision = m_realLossRevision;
                deliveryProof = currentProof;
                clearZeroParticipantEpoch = m_pendingRecoveryParticipants.empty();
            } else {
                const uint64_t precedingRevision = !currentProof || currentProof->empty()
                                                       ? std::numeric_limits<uint64_t>::max()
                                                       : m_realLossRevision;
                immediateDeliveryRequired =
                    m_tokenlessRecoveryObserved || m_deliveredProofRevision == precedingRevision;
                // A single adapter reset can kill more than one device domain. An earlier
                // tokenless submission failure identifies where submission first failed,
                // but it is not authority to reject later driver proof from another owned
                // domain. Accept every current-authority observation in this loss epoch.
                const DeadDeviceToken token(provenance, generation, deviceDomainId,
                                            m_deviceAuthorityEpoch);
                auto nextProof = currentProof
                                     ? std::make_shared<std::vector<DeadDeviceToken>>(*currentProof)
                                     : std::make_shared<std::vector<DeadDeviceToken>>();
                nextProof->push_back(token);
                m_realLossProof = std::move(nextProof);
                ++m_realLossRevision;
                acceptedRevision = m_realLossRevision;
                if (!m_realLossToken) m_realLossToken = token;
                if (m_pendingRecoveryParticipants.empty()) {
                    immediateDeliveryRequired = true;
                    clearZeroParticipantEpoch = true;
                }
                if (immediateDeliveryRequired) deliveryProof = m_realLossProof;
            }
        } else {
            if (replacementDeviceLoss)
                supersededGeneration = m_lossGeneration.load(std::memory_order_acquire);
            auto pendingParticipants = m_recoveryParticipants;
            auto cleanupAcknowledgedParticipants = m_terminalRecoveryParticipants;
            auto nextProof = std::make_shared<std::vector<DeadDeviceToken>>();
            nextProof->reserve(1);
#ifdef OLR_UNIT_TEST
            if (m_failNextLossPreparationForTest) {
                m_failNextLossPreparationForTest = false;
                throw std::bad_alloc();
            }
#endif
            generation = GpuGenerationCounter::instance().bump();
            const DeadDeviceToken token(provenance, generation, deviceDomainId,
                                        m_deviceAuthorityEpoch);
            nextProof->push_back(token);
            beginLossEpochLocked(generation, std::move(pendingParticipants),
                                 std::move(cleanupAcknowledgedParticipants));
            m_realLossToken = token;
            m_realLossProof = std::move(nextProof);
            ++m_realLossRevision;
            acceptedRevision = m_realLossRevision;
            m_lossCount.fetch_add(1, std::memory_order_acq_rel);
            m_undrained.fetch_add(1, std::memory_order_acq_rel);
            m_lost.store(true, std::memory_order_release);
            if (m_pendingRecoveryParticipants.empty()) {
                immediateDeliveryRequired = true;
                clearZeroParticipantEpoch = true;
                deliveryProof = m_realLossProof;
            }
        }
    }

#ifdef OLR_UNIT_TEST
    if (m_proofAcceptedForTest) {
        m_proofAcceptedForTest->release();
        if (m_continueProofDeliveryForTest) m_continueProofDeliveryForTest->acquire();
    }
#endif

    if (cleanupOnlyProof) {
        bool terminalProofPublished = false;
        try {
#ifdef OLR_UNIT_TEST
            if (m_failNextTerminalProofMergeForTest.exchange(false, std::memory_order_acq_rel))
                return 0;
#endif
            std::lock_guard<std::mutex> lock(m_epochMutex);
            auto nextProofs = m_terminalDeadDomainProofs;
            for (const uint64_t participantId : m_terminalRecoveryParticipants) {
                auto& retained = nextProofs[participantId];
                for (const DeadDeviceToken& token : acceptedProof) {
                    bool duplicate = false;
                    for (const DeadDeviceToken& existing : retained) {
                        if (existing.observedGeneration() == token.observedGeneration() &&
                            existing.deviceDomainId() == token.deviceDomainId() &&
                            existing.authorityEpoch() == token.authorityEpoch()) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) retained.push_back(token);
                }
            }
            m_terminalDeadDomainProofs.swap(nextProofs);
            terminalProofPublished = !m_terminalRecoveryParticipants.empty();
        } catch (...) {
            // Keep the roles and their prior proof unchanged so a later poll can retry.
            return 0;
        }
        GpuValidatedDeadDomains deadDomain(acceptedProof);
        return GpuRetireRegistry{}.abandonAllNoWait(deadDomain) > 0 || terminalProofPublished
                   ? generation
                   : 0;
    }

    // Initial proof delivery remains worker-owned. Once publication owns delivery,
    // carry the immutable full revision so a rejected older revision cannot be
    // skipped when a newer exact-domain proof arrives.
    if (immediateDeliveryRequired) {
        if (!deliveryProof || deliveryProof->empty()) return generation;
#ifdef OLR_UNIT_TEST
        m_lastImmediateProofSizeForTest = deliveryProof->size();
#endif
        GpuValidatedDeadDomains deadDomain(*deliveryProof);
        const GpuValidatedLossResult result =
            GpuRecoveryCoordinator::instance().coordinate(generation, acceptedRevision, [&]() {
                return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                              GpuRetireRegistry{}.abandonAllNoWait(deadDomain)};
            });
        if (result.status == GpuValidatedLossStatus::Completed) {
            std::lock_guard<std::mutex> lock(m_epochMutex);
            if (m_lossGeneration.load(std::memory_order_acquire) == generation &&
                m_realLossRevision == acceptedRevision) {
                m_deliveredProofRevision = acceptedRevision;
                if (clearZeroParticipantEpoch && m_pendingRecoveryParticipants.empty() &&
                    m_lost.load(std::memory_order_acquire)) {
                    beginRecoveryLocked(generation);
                    const uint64_t clearedGeneration = clearLossEpochLocked();
                    if (clearedGeneration > supersededGeneration)
                        supersededGeneration = clearedGeneration;
                }
            }
        }
    }
    deliveryLock.unlock();
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(supersededGeneration);
    return generation;
}

uint64_t GpuDeviceLossMonitor::recordSubmissionFailure(uintptr_t deviceDomainId) {
    // Submission/fence failure requires a rebuild, but is not proof that the
    // driver declared the device dead. Keep this epoch tokenless so recovery
    // uses bounded waits rather than the no-wait dead-device release path.
    (void) deviceDomainId;
    return recordTokenlessLoss();
}

std::optional<DeadDeviceToken> GpuDeviceLossMonitor::realLossToken() const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    return m_realLossToken;
}

std::vector<DeadDeviceToken> GpuDeviceLossMonitor::realLossTokens() const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    return m_realLossProof ? *m_realLossProof : std::vector<DeadDeviceToken>{};
}

bool GpuDeviceLossMonitor::consumeLossEvent() {
    uint64_t pending = m_undrained.load(std::memory_order_acquire);
    while (pending > 0 &&
           !m_undrained.compare_exchange_weak(pending, pending - 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
    }
    return pending > 0;
}

GpuRecoveryRegistration
GpuDeviceLossMonitor::registerRecoveryParticipantSnapshot(bool ownsCurrentDevice) {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    uint64_t participantId = m_nextRecoveryParticipantId++;
    if (participantId == 0) participantId = m_nextRecoveryParticipantId++;
    while (participantId == 0 || m_recoveryParticipants.count(participantId) != 0) {
        participantId = m_nextRecoveryParticipantId++;
        if (participantId == 0) participantId = m_nextRecoveryParticipantId++;
    }
    uint64_t lossGeneration = 0;
    bool primaryInserted = false;
    bool pendingInserted = false;
    bool acknowledgedInserted = false;
    try {
        primaryInserted = m_recoveryParticipants.insert(participantId).second;
#ifdef OLR_UNIT_TEST
        if (m_failRegistrationAfterPrimaryInsertForTest) {
            m_failRegistrationAfterPrimaryInsertForTest = false;
            throw std::bad_alloc();
        }
#endif
        if (m_lost.load(std::memory_order_acquire)) {
            lossGeneration = m_lossGeneration.load(std::memory_order_acquire);
            pendingInserted = m_pendingRecoveryParticipants.insert(participantId).second;
            if (!ownsCurrentDevice)
                acknowledgedInserted =
                    m_cleanupAcknowledgedParticipants.insert(participantId).second;
        }
    } catch (...) {
        if (acknowledgedInserted) m_cleanupAcknowledgedParticipants.erase(participantId);
        if (pendingInserted) m_pendingRecoveryParticipants.erase(participantId);
        if (primaryInserted) m_recoveryParticipants.erase(participantId);
        throw;
    }
    return GpuRecoveryRegistration(participantId, lossGeneration);
}

uint64_t GpuDeviceLossMonitor::registerRecoveryParticipant(bool ownsCurrentDevice) {
    return registerRecoveryParticipantSnapshot(ownsCurrentDevice).participantId();
}

uint64_t GpuDeviceLossMonitor::registerTerminalRecoveryParticipant() {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    uint64_t participantId = m_nextRecoveryParticipantId++;
    if (participantId == 0) participantId = m_nextRecoveryParticipantId++;
    while (participantId == 0 || m_recoveryParticipants.count(participantId) != 0) {
        participantId = m_nextRecoveryParticipantId++;
        if (participantId == 0) participantId = m_nextRecoveryParticipantId++;
    }
    bool primaryInserted = false;
    bool terminalInserted = false;
    bool proofInserted = false;
    bool pendingInserted = false;
    bool acknowledgedInserted = false;
    try {
        primaryInserted = m_recoveryParticipants.insert(participantId).second;
#ifdef OLR_UNIT_TEST
        if (m_failRegistrationAfterPrimaryInsertForTest) {
            m_failRegistrationAfterPrimaryInsertForTest = false;
            throw std::bad_alloc();
        }
#endif
        terminalInserted = m_terminalRecoveryParticipants.insert(participantId).second;
        proofInserted =
            m_terminalDeadDomainProofs.try_emplace(participantId, std::vector<DeadDeviceToken>{})
                .second;
        if (m_lost.load(std::memory_order_acquire)) {
            pendingInserted = m_pendingRecoveryParticipants.insert(participantId).second;
            acknowledgedInserted = m_cleanupAcknowledgedParticipants.insert(participantId).second;
        }
    } catch (...) {
        if (acknowledgedInserted) m_cleanupAcknowledgedParticipants.erase(participantId);
        if (pendingInserted) m_pendingRecoveryParticipants.erase(participantId);
        if (proofInserted) m_terminalDeadDomainProofs.erase(participantId);
        if (terminalInserted) m_terminalRecoveryParticipants.erase(participantId);
        if (primaryInserted) m_recoveryParticipants.erase(participantId);
        throw;
    }
    return participantId;
}

bool GpuDeviceLossMonitor::promoteRecoveryParticipantToTerminal(uint64_t participantId) noexcept {
    try {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        if (participantId == 0 || m_recoveryParticipants.count(participantId) == 0) return false;

        bool terminalInserted = false;
        bool acknowledgedInserted = false;
        try {
            terminalInserted = m_terminalRecoveryParticipants.insert(participantId).second;
#ifdef OLR_UNIT_TEST
            if (terminalInserted &&
                m_failNextTerminalPromotionForTest.exchange(false, std::memory_order_acq_rel))
                throw std::bad_alloc();
#endif
            if (m_lost.load(std::memory_order_acquire) &&
                m_pendingRecoveryParticipants.count(participantId) != 0) {
                acknowledgedInserted =
                    m_cleanupAcknowledgedParticipants.insert(participantId).second;
            }
        } catch (...) {
            if (acknowledgedInserted) m_cleanupAcknowledgedParticipants.erase(participantId);
            if (terminalInserted) m_terminalRecoveryParticipants.erase(participantId);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<qsizetype>
GpuDeviceLossMonitor::unregisterRecoveryParticipant(uint64_t participantId) noexcept {
    try {
        if (participantId == 0) return qsizetype(0);
        constexpr int kTokenlessDrainMs = 100;
        uint64_t retiredGeneration = 0;
        qsizetype abandoned = 0;
        std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        if (m_recoveryParticipants.count(participantId) == 0) {
            m_terminalRecoveryParticipants.erase(participantId);
            m_terminalDeadDomainProofs.erase(participantId);
            return qsizetype(0);
        }

        const bool pendingActiveEpoch = m_lost.load(std::memory_order_acquire) &&
                                        m_pendingRecoveryParticipants.count(participantId) != 0;
        if (!pendingActiveEpoch) {
#ifdef OLR_UNIT_TEST
            if (m_beforeNoLossUnregisterEraseForTest) {
                m_beforeNoLossUnregisterEraseForTest->release();
                if (m_continueNoLossUnregisterEraseForTest)
                    m_continueNoLossUnregisterEraseForTest->acquire();
            }
#endif
            m_recoveryParticipants.erase(participantId);
            m_terminalRecoveryParticipants.erase(participantId);
            m_terminalDeadDomainProofs.erase(participantId);
            m_pendingRecoveryParticipants.erase(participantId);
            m_cleanupAcknowledgedParticipants.erase(participantId);
            return qsizetype(0);
        }

        uint64_t lossGeneration = 0;
        uint64_t proofRevision = 0;
        uint64_t authorityEpoch = 0;
        std::shared_ptr<const std::vector<DeadDeviceToken>> proof;
        bool tokenless = false;
        lossGeneration = m_lossGeneration.load(std::memory_order_acquire);
        proofRevision = m_realLossRevision;
        authorityEpoch = m_rebuildInProgress && m_cleanupGeneration == lossGeneration
                             ? m_cleanupAuthorityEpoch
                             : m_deviceAuthorityEpoch;
        proof = m_realLossProof;
        tokenless = !proof || proof->empty();
        if (tokenless) m_tokenlessRecoveryObserved = true;
        epochLock.unlock();

        GpuValidatedLossResult cleanup;
        if (lossGeneration != 0) {
            bool proofValid = proof && !proof->empty();
            if (proof) {
                for (const DeadDeviceToken& token : *proof) {
                    if (token.observedGeneration() != lossGeneration ||
                        token.authorityEpoch() != authorityEpoch) {
                        proofValid = false;
                        break;
                    }
                }
            }
            GpuRetireRegistry registry;
            if (proofValid) {
                cleanup = GpuRecoveryCoordinator::instance().coordinate(
                    lossGeneration, proofRevision, [&]() {
                        GpuValidatedDeadDomains deadDomains(*proof);
                        return GpuValidatedLossResult{GpuValidatedLossStatus::Completed,
                                                      registry.abandonAllNoWait(deadDomains)};
                    });
            } else if (tokenless) {
                cleanup = GpuRecoveryCoordinator::instance().coordinate(
                    lossGeneration, std::numeric_limits<uint64_t>::max(), [&]() {
                        registry.drainWithBoundedWait(kTokenlessDrainMs);
                        return GpuValidatedLossResult{GpuValidatedLossStatus::Completed, 0};
                    });
            }
            if (cleanup.status == GpuValidatedLossStatus::Completed) abandoned = cleanup.abandoned;
        }

        epochLock.lock();
        if (cleanup.status != GpuValidatedLossStatus::Completed ||
            !m_lost.load(std::memory_order_acquire) ||
            m_lossGeneration.load(std::memory_order_acquire) != lossGeneration ||
            m_recoveryParticipants.count(participantId) == 0 ||
            m_pendingRecoveryParticipants.count(participantId) == 0) {
            return std::nullopt;
        }
        if (proof && !proof->empty() && m_realLossRevision == proofRevision)
            m_deliveredProofRevision = proofRevision;
        if (m_recoveryParticipants.erase(participantId) != 0) {
            m_terminalRecoveryParticipants.erase(participantId);
            m_terminalDeadDomainProofs.erase(participantId);
            const bool wasPending = m_pendingRecoveryParticipants.erase(participantId) != 0;
            m_cleanupAcknowledgedParticipants.erase(participantId);
            if (wasPending && m_lost.load(std::memory_order_acquire) &&
                m_pendingRecoveryParticipants.empty()) {
                const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
                beginRecoveryLocked(generation);
                retiredGeneration = clearLossEpochLocked();
            }
        }
        epochLock.unlock();
        GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
        return abandoned;
    } catch (...) {
        return std::nullopt;
    }
}

bool GpuDeviceLossMonitor::unregisterTerminalRecoveryParticipant(uint64_t participantId) noexcept {
    try {
        if (participantId == 0) return true;
#ifdef OLR_UNIT_TEST
        if (m_rejectNextTerminalUnregisterForTest.exchange(false, std::memory_order_acq_rel))
            return false;
#endif
        constexpr int kTokenlessDrainMs = 100;
        uint64_t retiredGeneration = 0;
        std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        if (m_recoveryParticipants.count(participantId) == 0) {
            m_terminalRecoveryParticipants.erase(participantId);
            m_terminalDeadDomainProofs.erase(participantId);
            return true;
        }

        const bool pendingActiveEpoch = m_lost.load(std::memory_order_acquire) &&
                                        m_pendingRecoveryParticipants.count(participantId) != 0;
        if (pendingActiveEpoch) {
            const uint64_t lossGeneration = m_lossGeneration.load(std::memory_order_acquire);
            const uint64_t proofRevision = m_realLossRevision;
            const uint64_t authorityEpoch =
                m_rebuildInProgress && m_cleanupGeneration == lossGeneration
                    ? m_cleanupAuthorityEpoch
                    : m_deviceAuthorityEpoch;
            const std::shared_ptr<const std::vector<DeadDeviceToken>> proof = m_realLossProof;
            bool proofValid = proof && !proof->empty();
            if (proof) {
                for (const DeadDeviceToken& token : *proof) {
                    if (token.observedGeneration() != lossGeneration ||
                        token.authorityEpoch() != authorityEpoch) {
                        proofValid = false;
                        break;
                    }
                }
            }
            if (!proofValid) m_tokenlessRecoveryObserved = true;
            epochLock.unlock();
            GpuRetireRegistry registry;
            if (proofValid) {
                GpuValidatedDeadDomains deadDomains(*proof);
                (void) registry.abandonAllNoWait(deadDomains);
            } else {
                (void) registry.drainWithBoundedWait(kTokenlessDrainMs);
            }
            epochLock.lock();
            if (m_lossGeneration.load(std::memory_order_acquire) != lossGeneration ||
                m_pendingRecoveryParticipants.count(participantId) == 0) {
                return false;
            }
            if (proofValid && m_realLossRevision == proofRevision)
                m_deliveredProofRevision = proofRevision;
        }

        const bool wasPending = m_pendingRecoveryParticipants.erase(participantId) != 0;
        m_cleanupAcknowledgedParticipants.erase(participantId);
        m_recoveryParticipants.erase(participantId);
        m_terminalRecoveryParticipants.erase(participantId);
        m_terminalDeadDomainProofs.erase(participantId);
        if (wasPending && m_lost.load(std::memory_order_acquire) &&
            m_pendingRecoveryParticipants.empty()) {
            const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
            beginRecoveryLocked(generation);
            retiredGeneration = clearLossEpochLocked();
        }
        epochLock.unlock();
        GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
        return true;
    } catch (...) {
        return false;
    }
}

bool GpuDeviceLossMonitor::acknowledgeRecoveryCleanup(uint64_t participantId,
                                                      uint64_t lossGeneration) {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (participantId == 0 || lossGeneration == 0 ||
        m_lossGeneration.load(std::memory_order_acquire) != lossGeneration ||
        !m_lost.load(std::memory_order_acquire) ||
        m_pendingRecoveryParticipants.count(participantId) == 0 ||
        (m_realLossProof && !m_realLossProof->empty() &&
         m_deliveredProofRevision != m_realLossRevision))
        return false;
    m_cleanupAcknowledgedParticipants.insert(participantId);
    return true;
}

GpuRecoveryTicket GpuDeviceLossMonitor::beginRebuild(uint64_t participantId) {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
    if (participantId == 0 || generation == 0 || !m_lost.load(std::memory_order_acquire) ||
        m_recoveryParticipants.count(participantId) == 0 ||
        m_terminalRecoveryParticipants.count(participantId) != 0 ||
        m_pendingRecoveryParticipants.count(participantId) == 0 ||
        m_cleanupAcknowledgedParticipants.size() != m_pendingRecoveryParticipants.size() ||
        (m_realLossProof && !m_realLossProof->empty() &&
         m_deliveredProofRevision != m_realLossRevision))
        return GpuRecoveryTicket(0, 0, 0, 0, 0);
    beginRecoveryLocked(generation);
    return GpuRecoveryTicket(generation, m_realLossRevision, m_recoveryAuthorityEpoch,
                             m_recoveryRevision, participantId);
}

bool GpuDeviceLossMonitor::clearForRebuild(const GpuRecoveryTicket& ticket) {
    if (!ticket.isValid()) return false;
    try {
        uint64_t retiredGeneration = 0;
        {
            std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
            std::lock_guard<std::mutex> lock(m_epochMutex);
            const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
            if (!m_lost.load(std::memory_order_acquire) || generation != ticket.m_lossGeneration ||
                m_recoveryGeneration != ticket.m_lossGeneration ||
                m_recoveryRevision != ticket.m_recoveryRevision ||
                m_recoveryAuthorityEpoch != ticket.m_authorityEpoch ||
                ticket.m_proofRevision != m_realLossRevision ||
                (m_realLossProof && !m_realLossProof->empty() &&
                 m_deliveredProofRevision != m_realLossRevision) ||
                m_recoveryParticipants.count(ticket.m_participantId) == 0 ||
                m_terminalRecoveryParticipants.count(ticket.m_participantId) != 0 ||
                m_pendingRecoveryParticipants.count(ticket.m_participantId) == 0)
                return false;
            if (!snapshotTerminalProofAndRemovePendingLocked()) return false;
            if (m_pendingRecoveryParticipants.erase(ticket.m_participantId) == 0) return false;
            m_cleanupAcknowledgedParticipants.erase(ticket.m_participantId);
            if (m_pendingRecoveryParticipants.empty()) retiredGeneration = clearLossEpochLocked();
        }
        GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
        return true;
    } catch (...) {
        return false;
    }
}

void GpuDeviceLossMonitor::beginRebuild() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
    if (generation != 0 && m_lost.load(std::memory_order_acquire) &&
        (!m_realLossProof || m_realLossProof->empty() ||
         m_deliveredProofRevision == m_realLossRevision))
        beginRecoveryLocked(generation);
}

void GpuDeviceLossMonitor::clearForRebuild() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    uint64_t retiredGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        bool hasNonTerminalPending = false;
        for (const uint64_t participantId : m_pendingRecoveryParticipants) {
            if (m_terminalRecoveryParticipants.count(participantId) == 0) {
                hasNonTerminalPending = true;
                break;
            }
        }
        if (generation == 0 || !m_lost.load(std::memory_order_acquire) || hasNonTerminalPending ||
            (m_realLossProof && !m_realLossProof->empty() &&
             m_deliveredProofRevision != m_realLossRevision))
            return;
        beginRecoveryLocked(generation);
        if (!snapshotTerminalProofAndRemovePendingLocked()) return;
        if (!m_pendingRecoveryParticipants.empty()) return;
        retiredGeneration = clearLossEpochLocked();
    }
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
}

void GpuDeviceLossMonitor::reset() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    uint64_t retiredGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        retiredGeneration = m_lossGeneration.load(std::memory_order_acquire);
        if (++m_deviceAuthorityEpoch == 0) ++m_deviceAuthorityEpoch;
        m_publishedDeviceAuthorityEpoch.store(m_deviceAuthorityEpoch, std::memory_order_release);
        m_rebuildInProgress = false;
        m_lost.store(false, std::memory_order_release);
        m_lossCount.store(0, std::memory_order_release);
        m_undrained.store(0, std::memory_order_release);
        m_lossGeneration.store(0, std::memory_order_release);
        m_realLossToken.reset();
        m_realLossProof.reset();
        m_realLossRevision = 0;
        m_deliveredProofRevision = 0;
        m_tokenlessRecoveryObserved = false;
        m_recoveryParticipants.clear();
        m_terminalRecoveryParticipants.clear();
        m_terminalDeadDomainProofs.clear();
        m_pendingRecoveryParticipants.clear();
        m_cleanupAcknowledgedParticipants.clear();
        m_recoveryGeneration = 0;
        m_recoveryRevision = 0;
        m_recoveryAuthorityEpoch = 0;
        m_cleanupAuthorityEpoch = 0;
        m_cleanupGeneration = 0;
        m_nextRecoveryParticipantId = 1;
#ifdef OLR_UNIT_TEST
        m_failNextLossPreparationForTest = false;
        m_failRegistrationAfterPrimaryInsertForTest = false;
        m_rejectNextTerminalUnregisterForTest.store(false, std::memory_order_release);
        m_failNextTerminalProofMergeForTest.store(false, std::memory_order_release);
        m_failNextTerminalPromotionForTest.store(false, std::memory_order_release);
        m_lastImmediateProofSizeForTest = 0;
#endif
    }
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
#ifdef OLR_UNIT_TEST
    GpuRecoveryCoordinator::instance().resetForTest();
#endif
}
