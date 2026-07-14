#include "playback/gpu/gpurecoverycoordinator.h"

GpuRecoveryCoordinator& GpuRecoveryCoordinator::instance() {
    static GpuRecoveryCoordinator coordinator;
    return coordinator;
}

GpuValidatedLossResult
GpuRecoveryCoordinator::coordinate(uint64_t lossGeneration, uint64_t proofRevision,
                                   const std::function<GpuValidatedLossResult()>& leaderWork) {
    if (lossGeneration == 0 || proofRevision == 0 || !leaderWork) return {};

    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_completedGeneration == lossGeneration && m_completedRevision == proofRevision) {
        GpuValidatedLossResult follower = m_completedResult;
        follower.coordinatorLeader = false;
        return follower;
    }
    if (m_inProgress) {
        if (m_activeGeneration != lossGeneration || m_activeRevision != proofRevision) return {};
        m_finished.wait(lock, [&] { return !m_inProgress; });
        if (m_completedGeneration != lossGeneration || m_completedRevision != proofRevision)
            return {};
        GpuValidatedLossResult follower = m_completedResult;
        follower.coordinatorLeader = false;
        return follower;
    }

    m_inProgress = true;
    m_activeGeneration = lossGeneration;
    m_activeRevision = proofRevision;
    lock.unlock();

    GpuValidatedLossResult result;
    try {
        result = leaderWork();
    } catch (...) {
        result = {};
    }

    lock.lock();
    m_completedGeneration = lossGeneration;
    m_completedRevision = proofRevision;
    m_completedResult = result;
    m_completedResult.coordinatorLeader = false;
    m_activeGeneration = 0;
    m_activeRevision = 0;
    m_inProgress = false;
    lock.unlock();
    m_finished.notify_all();
    result.coordinatorLeader = true;
    return result;
}

bool GpuRecoveryCoordinator::completed(uint64_t lossGeneration, uint64_t proofRevision) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_inProgress && m_completedGeneration == lossGeneration &&
           m_completedRevision == proofRevision &&
           m_completedResult.status == GpuValidatedLossStatus::Completed;
}

#ifdef OLR_UNIT_TEST
void GpuRecoveryCoordinator::resetForTest() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inProgress) return;
    m_activeGeneration = 0;
    m_activeRevision = 0;
    m_completedGeneration = 0;
    m_completedRevision = 0;
    m_completedResult = {};
}
#endif
