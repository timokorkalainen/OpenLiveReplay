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

bool GpuDeviceLossMonitor::isLost() const {
    return m_lost.load(std::memory_order_acquire);
}

uint64_t GpuDeviceLossMonitor::lossCount() const {
    return m_lossCount.load(std::memory_order_acquire);
}

uint64_t GpuDeviceLossMonitor::recordLoss() {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (m_lost.load(std::memory_order_acquire)) {
        return m_lossGeneration.load(std::memory_order_acquire);
    }

    const uint64_t generation = GpuGenerationCounter::instance().bump();
    m_lossGeneration.store(generation, std::memory_order_release);
    m_lossCount.fetch_add(1, std::memory_order_acq_rel);
    m_undrained.fetch_add(1, std::memory_order_acq_rel);
    m_lost.store(true, std::memory_order_release);
    return generation;
}

uint64_t GpuDeviceLossMonitor::captureDeviceAuthorityEpoch() const {
    std::lock_guard<std::mutex> lock(m_epochMutex);
    return m_deviceAuthorityEpoch;
}

uint64_t GpuDeviceLossMonitor::publishRealDeviceLoss(DeadDeviceToken::Provenance provenance,
                                                     uint64_t deviceAuthorityEpoch,
                                                     uintptr_t deviceDomainId) {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    uint64_t generation = 0;
    uint64_t acceptedRevision = 0;
    std::vector<DeadDeviceToken> acceptedProof;
    bool immediateDeliveryRequired = false;
    {
        std::lock_guard<std::mutex> lock(m_epochMutex);
        if (deviceAuthorityEpoch != m_deviceAuthorityEpoch) return 0;
        if (m_lost.load(std::memory_order_acquire)) {
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
        } else {
            generation = GpuGenerationCounter::instance().bump();
            m_lossGeneration.store(generation, std::memory_order_release);
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
        }
    }

#ifdef OLR_UNIT_TEST
    if (m_proofAcceptedForTest) {
        m_proofAcceptedForTest->release();
        if (m_continueProofDeliveryForTest) m_continueProofDeliveryForTest->acquire();
    }
#endif

    // Initial proof delivery remains worker-owned. Once tokenless recovery has
    // observed the epoch or a preceding proof revision has been delivered,
    // publication is the only guaranteed production event for a newly accepted
    // domain. Carry only that proof out of the epoch lock and release it without
    // waiting; m_proofDeliveryMutex prevents rebuild/clear from overtaking this
    // cold-path handoff.
    if (immediateDeliveryRequired) {
        GpuValidatedDeadDomains deadDomain(acceptedProof);
        (void) GpuRetireRegistry{}.abandonAllNoWait(deadDomain);
        std::lock_guard<std::mutex> lock(m_epochMutex);
        if (m_lossGeneration.load(std::memory_order_acquire) == generation &&
            m_realLossRevision == acceptedRevision)
            m_deliveredProofRevision = acceptedRevision;
    }
    return generation;
}

uint64_t GpuDeviceLossMonitor::recordSubmissionFailure(uintptr_t deviceDomainId) {
    // Submission/fence failure requires a rebuild, but is not proof that the
    // driver declared the device dead. Keep this epoch tokenless so recovery
    // uses bounded waits rather than the no-wait dead-device release path.
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (m_lost.load(std::memory_order_acquire))
        return m_lossGeneration.load(std::memory_order_acquire);
    const uint64_t generation = GpuGenerationCounter::instance().bump();
    m_lossGeneration.store(generation, std::memory_order_release);
    (void) deviceDomainId;
    m_lossCount.fetch_add(1, std::memory_order_acq_rel);
    m_undrained.fetch_add(1, std::memory_order_acq_rel);
    m_lost.store(true, std::memory_order_release);
    return generation;
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

void GpuDeviceLossMonitor::beginRebuild() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    if (++m_deviceAuthorityEpoch == 0) ++m_deviceAuthorityEpoch;
    m_publishedDeviceAuthorityEpoch.store(m_deviceAuthorityEpoch, std::memory_order_release);
    m_rebuildInProgress = true;
}

void GpuDeviceLossMonitor::clearForRebuild() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
    // Preserve compatibility with direct clear callers while allowing production
    // rebuilds to mint replacement-device authority between begin and commit.
    if (!m_rebuildInProgress && ++m_deviceAuthorityEpoch == 0) ++m_deviceAuthorityEpoch;
    m_publishedDeviceAuthorityEpoch.store(m_deviceAuthorityEpoch, std::memory_order_release);
    m_rebuildInProgress = false;
    m_lost.store(false, std::memory_order_release);
    m_lossGeneration.store(0, std::memory_order_release);
    m_realLossToken.reset();
    m_realLossTokens.clear();
    m_realLossRevision = 0;
    m_deliveredProofRevision = 0;
    m_tokenlessRecoveryObserved = false;
}

void GpuDeviceLossMonitor::reset() {
    std::lock_guard<std::mutex> deliveryLock(m_proofDeliveryMutex);
    std::lock_guard<std::mutex> lock(m_epochMutex);
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
#ifdef OLR_UNIT_TEST
    GpuRecoveryCoordinator::instance().resetForTest();
#endif
}
