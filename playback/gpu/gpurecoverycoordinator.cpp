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
    if (lossGeneration <= m_retiredGeneration) return {};
    const auto key = std::make_pair(lossGeneration, proofRevision);
    std::shared_ptr<RecoveryState>& stateSlot = m_recoveries[key];
    if (!stateSlot) stateSlot = std::make_shared<RecoveryState>();
    const std::shared_ptr<RecoveryState> state = stateSlot;
    if (state->completed) {
        GpuValidatedLossResult follower = state->result;
        follower.coordinatorLeader = false;
        return follower;
    }
    if (state->inProgress) {
        state->finished.wait(lock, [&] { return state->completed; });
        GpuValidatedLossResult follower = state->result;
        follower.coordinatorLeader = false;
        return follower;
    }

    state->inProgress = true;
    lock.unlock();

    GpuValidatedLossResult result;
    try {
        result = leaderWork();
    } catch (...) {
        result = {};
    }

    lock.lock();
    state->result = result;
    state->result.coordinatorLeader = false;
    state->inProgress = false;
    state->completed = true;
    if (lossGeneration <= m_retiredGeneration) {
        const auto current = m_recoveries.find(key);
        if (current != m_recoveries.end() && current->second == state) m_recoveries.erase(current);
    }
    lock.unlock();
    state->finished.notify_all();
    result.coordinatorLeader = true;
    return result;
}

void GpuRecoveryCoordinator::retireGenerationsThrough(uint64_t lossGeneration) {
    if (lossGeneration == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (lossGeneration > m_retiredGeneration) m_retiredGeneration = lossGeneration;
    for (auto recovery = m_recoveries.begin(); recovery != m_recoveries.end();) {
        const std::shared_ptr<RecoveryState>& state = recovery->second;
        if (recovery->first.first <= m_retiredGeneration && state && state->completed)
            recovery = m_recoveries.erase(recovery);
        else
            ++recovery;
    }
}

#ifdef OLR_UNIT_TEST
void GpuRecoveryCoordinator::resetForTest() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [key, state] : m_recoveries) {
        (void) key;
        if (state && state->inProgress) return;
    }
    m_recoveries.clear();
    m_retiredGeneration = 0;
}

std::size_t GpuRecoveryCoordinator::cachedRecoveryCountForTest() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_recoveries.size();
}
#endif
