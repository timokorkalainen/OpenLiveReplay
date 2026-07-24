#include "playback/gpu/gpurecoverycoordinator.h"

#ifdef OLR_UNIT_TEST
#include <QSemaphore>
#endif

GpuRecoveryCoordinator& GpuRecoveryCoordinator::instance() {
    static GpuRecoveryCoordinator coordinator;
    return coordinator;
}

GpuValidatedLossResult GpuRecoveryCoordinator::coordinateImpl(uint64_t lossGeneration,
                                                              uint64_t proofRevision,
                                                              void* leaderContext,
                                                              LeaderWork leaderWork) noexcept {
    try {
        if (lossGeneration == 0 || proofRevision == 0 || !leaderContext || !leaderWork) return {};

        std::unique_lock<std::mutex> lock(m_mutex);
        if (lossGeneration <= m_retiredGeneration) return {};
        const auto key = std::make_pair(lossGeneration, proofRevision);
        auto recovery = m_recoveries.find(key);
        if (recovery == m_recoveries.end()) {
#ifdef OLR_UNIT_TEST
            if (m_failNextAdmission) {
                m_failNextAdmission = false;
                throw std::bad_alloc();
            }
#endif
            // Allocate the state before mutating the map. If allocation fails,
            // admission leaves no unreachable null entry behind.
            auto newState = std::make_shared<RecoveryState>();
            recovery = m_recoveries.emplace(key, std::move(newState)).first;
        }
        std::shared_ptr<RecoveryState> state = recovery->second;
        if (state->completed.load(std::memory_order_acquire)) {
            GpuValidatedLossResult follower = state->result;
            if (follower.status == GpuValidatedLossStatus::Completed) {
                follower.coordinatorLeader = false;
                return follower;
            }
            // A rejected attempt is immutable once published. Its notified
            // followers retain this completed state while the retry receives a
            // fresh identity, so no waiter can be captured by the new attempt.
            auto retryState = std::make_shared<RecoveryState>();
            recovery->second = retryState;
            state = std::move(retryState);
        }
        if (state->inProgress.load(std::memory_order_acquire)) {
#ifdef OLR_UNIT_TEST
            if (m_followerWaitingForTest) m_followerWaitingForTest->release();
#endif
            state->finished.wait(lock,
                                 [&] { return state->completed.load(std::memory_order_acquire); });
            GpuValidatedLossResult follower = state->result;
            follower.coordinatorLeader = false;
            return follower;
        }

        state->inProgress.store(true, std::memory_order_release);
        lock.unlock();

        GpuValidatedLossResult result = leaderWork(leaderContext);

        state->result = result;
        state->result.coordinatorLeader = false;
        state->inProgress.store(false, std::memory_order_relaxed);
        state->completed.store(true, std::memory_order_release);
#ifdef OLR_UNIT_TEST
        if (result.status == GpuValidatedLossStatus::Rejected &&
            m_afterRejectedPublicationForTest) {
            m_afterRejectedPublicationForTest->release();
            if (m_continueRejectedNotificationForTest)
                m_continueRejectedNotificationForTest->acquire();
        }
#endif
        state->finished.notify_all();

        // Cleanup is deliberately after publication/notification. A native
        // mutex failure can leave a cache entry behind, but can no longer leave
        // followers blocked; a rejected entry is also retryable in place.
        try {
            lock.lock();
            if (result.status == GpuValidatedLossStatus::Rejected ||
                lossGeneration <= m_retiredGeneration) {
                const auto current = m_recoveries.find(key);
                if (current != m_recoveries.end() && current->second == state &&
                    state->completed.load(std::memory_order_acquire) &&
                    !state->inProgress.load(std::memory_order_acquire))
                    m_recoveries.erase(current);
            }
            lock.unlock();
        } catch (...) {
            // Publication already woke followers; cache cleanup is best-effort.
            static_cast<void>(0);
        }
        result.coordinatorLeader = true;
        return result;
    } catch (...) {
        return {};
    }
}

void GpuRecoveryCoordinator::retireGenerationsThrough(uint64_t lossGeneration) noexcept {
    try {
        if (lossGeneration == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (lossGeneration > m_retiredGeneration) m_retiredGeneration = lossGeneration;
        for (auto recovery = m_recoveries.begin(); recovery != m_recoveries.end();) {
            const std::shared_ptr<RecoveryState>& state = recovery->second;
            if (recovery->first.first <= m_retiredGeneration && state &&
                state->completed.load(std::memory_order_acquire))
                recovery = m_recoveries.erase(recovery);
            else
                ++recovery;
        }
    } catch (...) {
        // Generation retirement is best-effort and must not escape teardown.
        static_cast<void>(0);
    }
}

#ifdef OLR_UNIT_TEST
void GpuRecoveryCoordinator::resetForTest() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [key, state] : m_recoveries) {
        (void) key;
        if (state && state->inProgress.load(std::memory_order_acquire)) return;
    }
    m_recoveries.clear();
    m_retiredGeneration = 0;
    m_failNextAdmission = false;
    m_followerWaitingForTest = nullptr;
    m_afterRejectedPublicationForTest = nullptr;
    m_continueRejectedNotificationForTest = nullptr;
}

void GpuRecoveryCoordinator::failNextAdmissionForTest() noexcept {
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failNextAdmission = true;
    } catch (...) {
        // Fault-injection setup is best-effort in a noexcept test hook.
        static_cast<void>(0);
    }
}

std::size_t GpuRecoveryCoordinator::cachedRecoveryCountForTest() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_recoveries.size();
}
#endif
