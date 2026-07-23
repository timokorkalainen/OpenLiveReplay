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

void GpuDeviceLossMonitor::beginLossEpochLocked(uint64_t generation) {
    m_rebuildInProgress = false;
    m_realLossToken.reset();
    m_realLossTokens.clear();
    m_realLossRevision = 0;
    m_deliveredProofRevision = 0;
    m_tokenlessRecoveryObserved = false;
    m_pendingRecoveryParticipants = m_recoveryParticipants;
    m_cleanupAcknowledgedParticipants.clear();
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
    m_realLossTokens.clear();
    m_realLossRevision = 0;
    m_deliveredProofRevision = 0;
    m_tokenlessRecoveryObserved = false;
    m_pendingRecoveryParticipants.clear();
    m_cleanupAcknowledgedParticipants.clear();
    m_recoveryGeneration = 0;
    m_recoveryAuthorityEpoch = 0;
    return retiredGeneration;
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
            return m_lossGeneration.load(std::memory_order_acquire);
        }

        generation = GpuGenerationCounter::instance().bump();
        beginLossEpochLocked(generation);
        m_lossCount.fetch_add(1, std::memory_order_acq_rel);
        m_undrained.fetch_add(1, std::memory_order_acq_rel);
        m_lost.store(true, std::memory_order_release);
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
    bool immediateDeliveryRequired = false;
    bool cleanupOnlyProof = false;
    bool clearZeroParticipantEpoch = false;
    {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        if (deviceAuthorityEpoch != m_deviceAuthorityEpoch) {
            generation = deviceAuthorityEpoch == m_cleanupAuthorityEpoch && m_cleanupGeneration != 0
                             ? m_cleanupGeneration
                             : GpuGenerationCounter::instance().current();
            acceptedProof.push_back(
                DeadDeviceToken(provenance, generation, deviceDomainId, deviceAuthorityEpoch));
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
            for (const DeadDeviceToken& token : m_realLossTokens) {
                if (token.deviceDomainId() == deviceDomainId) return generation;
            }
            const uint64_t precedingRevision = m_realLossTokens.empty()
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
            acceptedProof.push_back(token);
            m_realLossTokens.push_back(token);
            ++m_realLossRevision;
            acceptedRevision = m_realLossRevision;
            if (!m_realLossToken) m_realLossToken = token;
            if (m_pendingRecoveryParticipants.empty()) {
                immediateDeliveryRequired = true;
                clearZeroParticipantEpoch = true;
            }
        } else {
            if (replacementDeviceLoss)
                supersededGeneration = m_lossGeneration.load(std::memory_order_acquire);
            generation = GpuGenerationCounter::instance().bump();
            beginLossEpochLocked(generation);
            const DeadDeviceToken token(provenance, generation, deviceDomainId,
                                        m_deviceAuthorityEpoch);
            acceptedProof.push_back(token);
            m_realLossToken = token;
            m_realLossTokens = {token};
            ++m_realLossRevision;
            acceptedRevision = m_realLossRevision;
            m_lossCount.fetch_add(1, std::memory_order_acq_rel);
            m_undrained.fetch_add(1, std::memory_order_acq_rel);
            m_lost.store(true, std::memory_order_release);
            if (m_pendingRecoveryParticipants.empty()) {
                immediateDeliveryRequired = true;
                clearZeroParticipantEpoch = true;
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
        GpuValidatedDeadDomains deadDomain(acceptedProof);
        return GpuRetireRegistry{}.abandonAllNoWait(deadDomain) > 0 ? generation : 0;
    }

    // Initial proof delivery remains worker-owned. Once tokenless recovery has
    // observed the epoch or a preceding proof revision has been delivered,
    // publication is the only guaranteed production event for a newly accepted
    // domain. Carry only that proof out of the epoch lock and release it without
    // waiting; m_proofDeliveryMutex prevents rebuild/clear from overtaking this
    // cold-path handoff.
    if (immediateDeliveryRequired) {
        GpuValidatedDeadDomains deadDomain(acceptedProof);
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
    return m_realLossTokens;
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
    m_recoveryParticipants.insert(participantId);
    uint64_t lossGeneration = 0;
    if (m_lost.load(std::memory_order_acquire)) {
        lossGeneration = m_lossGeneration.load(std::memory_order_acquire);
        m_pendingRecoveryParticipants.insert(participantId);
        if (!ownsCurrentDevice) m_cleanupAcknowledgedParticipants.insert(participantId);
    }
    return GpuRecoveryRegistration(participantId, lossGeneration);
}

uint64_t GpuDeviceLossMonitor::registerRecoveryParticipant(bool ownsCurrentDevice) {
    return registerRecoveryParticipantSnapshot(ownsCurrentDevice).participantId();
}

std::optional<qsizetype>
GpuDeviceLossMonitor::unregisterRecoveryParticipant(uint64_t participantId) {
    if (participantId == 0) return qsizetype(0);
    constexpr int kTokenlessDrainMs = 100;
    uint64_t retiredGeneration = 0;
    qsizetype abandoned = 0;
    {
        std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
        std::unique_lock<std::mutex> epochLock(m_epochMutex);
        if (m_recoveryParticipants.count(participantId) == 0) return qsizetype(0);

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
            m_pendingRecoveryParticipants.erase(participantId);
            m_cleanupAcknowledgedParticipants.erase(participantId);
            return qsizetype(0);
        }

        uint64_t lossGeneration = 0;
        uint64_t proofRevision = 0;
        uint64_t authorityEpoch = 0;
        std::vector<DeadDeviceToken> proof;
        bool tokenless = false;
        lossGeneration = m_lossGeneration.load(std::memory_order_acquire);
        proofRevision = m_realLossRevision;
        authorityEpoch = m_rebuildInProgress && m_cleanupGeneration == lossGeneration
                             ? m_cleanupAuthorityEpoch
                             : m_deviceAuthorityEpoch;
        proof = m_realLossTokens;
        tokenless = proof.empty();
        if (tokenless) m_tokenlessRecoveryObserved = true;
        epochLock.unlock();

        GpuValidatedLossResult cleanup;
        if (lossGeneration != 0) {
            bool proofValid = !proof.empty();
            for (const DeadDeviceToken& token : proof) {
                if (token.observedGeneration() != lossGeneration ||
                    token.authorityEpoch() != authorityEpoch) {
                    proofValid = false;
                    break;
                }
            }
            GpuRetireRegistry registry;
            if (proofValid) {
                cleanup = GpuRecoveryCoordinator::instance().coordinate(
                    lossGeneration, proofRevision, [&]() {
                        GpuValidatedDeadDomains deadDomains(proof);
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
        if (!proof.empty() && m_realLossRevision == proofRevision)
            m_deliveredProofRevision = proofRevision;
        if (m_recoveryParticipants.erase(participantId) != 0) {
            const bool wasPending = m_pendingRecoveryParticipants.erase(participantId) != 0;
            m_cleanupAcknowledgedParticipants.erase(participantId);
            if (wasPending && m_lost.load(std::memory_order_acquire) &&
                m_pendingRecoveryParticipants.empty()) {
                const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
                beginRecoveryLocked(generation);
                retiredGeneration = clearLossEpochLocked();
            }
        }
    }
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
    return abandoned;
}

bool GpuDeviceLossMonitor::acknowledgeRecoveryCleanup(uint64_t participantId,
                                                      uint64_t lossGeneration) {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (participantId == 0 || lossGeneration == 0 ||
        m_lossGeneration.load(std::memory_order_acquire) != lossGeneration ||
        !m_lost.load(std::memory_order_acquire) ||
        m_pendingRecoveryParticipants.count(participantId) == 0)
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
        m_pendingRecoveryParticipants.count(participantId) == 0 ||
        m_cleanupAcknowledgedParticipants.size() != m_pendingRecoveryParticipants.size())
        return GpuRecoveryTicket(0, 0, 0, 0, 0);
    beginRecoveryLocked(generation);
    return GpuRecoveryTicket(generation, m_realLossRevision, m_recoveryAuthorityEpoch,
                             m_recoveryRevision, participantId);
}

bool GpuDeviceLossMonitor::clearForRebuild(const GpuRecoveryTicket& ticket) {
    if (!ticket.isValid()) return false;
    uint64_t retiredGeneration = 0;
    {
        std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
        std::lock_guard<std::mutex> lock(m_epochMutex);
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        if (!m_lost.load(std::memory_order_acquire) || generation != ticket.m_lossGeneration ||
            m_recoveryGeneration != ticket.m_lossGeneration ||
            m_recoveryRevision != ticket.m_recoveryRevision ||
            m_recoveryAuthorityEpoch != ticket.m_authorityEpoch ||
            ticket.m_proofRevision > m_realLossRevision ||
            m_recoveryParticipants.count(ticket.m_participantId) == 0 ||
            m_pendingRecoveryParticipants.erase(ticket.m_participantId) == 0)
            return false;
        if (m_pendingRecoveryParticipants.empty()) retiredGeneration = clearLossEpochLocked();
    }
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
    return true;
}

void GpuDeviceLossMonitor::beginRebuild() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
    if (generation != 0 && m_lost.load(std::memory_order_acquire)) beginRecoveryLocked(generation);
}

void GpuDeviceLossMonitor::clearForRebuild() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    uint64_t retiredGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        const uint64_t generation = m_lossGeneration.load(std::memory_order_acquire);
        if (generation == 0 || !m_lost.load(std::memory_order_acquire) ||
            !m_pendingRecoveryParticipants.empty())
            return;
        beginRecoveryLocked(generation);
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
        m_realLossTokens.clear();
        m_realLossRevision = 0;
        m_deliveredProofRevision = 0;
        m_tokenlessRecoveryObserved = false;
        m_recoveryParticipants.clear();
        m_pendingRecoveryParticipants.clear();
        m_cleanupAcknowledgedParticipants.clear();
        m_recoveryGeneration = 0;
        m_recoveryRevision = 0;
        m_recoveryAuthorityEpoch = 0;
        m_cleanupAuthorityEpoch = 0;
        m_cleanupGeneration = 0;
        m_nextRecoveryParticipantId = 1;
    }
    GpuRecoveryCoordinator::instance().retireGenerationsThrough(retiredGeneration);
#ifdef OLR_UNIT_TEST
    GpuRecoveryCoordinator::instance().resetForTest();
#endif
}
