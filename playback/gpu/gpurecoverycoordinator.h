#ifndef OLR_GPURECOVERYCOORDINATOR_H
#define OLR_GPURECOVERYCOORDINATOR_H

#include <QtGlobal>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

class QSemaphore;

enum class GpuValidatedLossStatus : uint8_t { Rejected, Completed };

struct GpuValidatedLossResult {
    GpuValidatedLossStatus status = GpuValidatedLossStatus::Rejected;
    qsizetype abandoned = 0;
    bool coordinatorLeader = false;
};

class GpuRecoveryCoordinator final {
public:
    static GpuRecoveryCoordinator& instance();
    template <typename Fn>
    GpuValidatedLossResult coordinate(uint64_t lossGeneration, uint64_t proofRevision,
                                      Fn&& leaderWork) noexcept {
        using Work = std::remove_reference_t<Fn>;
        auto invoke = [](void* context) noexcept {
            try {
                return std::invoke(*static_cast<Work*>(context));
            } catch (...) {
                return GpuValidatedLossResult{};
            }
        };
        return coordinateImpl(lossGeneration, proofRevision, std::addressof(leaderWork), invoke);
    }
    void retireGenerationsThrough(uint64_t lossGeneration) noexcept;
#ifdef OLR_UNIT_TEST
    void resetForTest();
    std::size_t cachedRecoveryCountForTest();
    void failNextAdmissionForTest() noexcept;
#endif

private:
    GpuRecoveryCoordinator() = default;
#ifdef OLR_UNIT_TEST
    friend class TestDeviceLossMonitor;
#endif

    using LeaderWork = GpuValidatedLossResult (*)(void*) noexcept;
    GpuValidatedLossResult coordinateImpl(uint64_t lossGeneration, uint64_t proofRevision,
                                          void* leaderContext, LeaderWork leaderWork) noexcept;

    struct RecoveryState {
        std::condition_variable finished;
        std::atomic<bool> inProgress{false};
        std::atomic<bool> completed{false};
        GpuValidatedLossResult result;
    };

    std::mutex m_mutex;
    std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<RecoveryState>> m_recoveries;
    uint64_t m_retiredGeneration = 0;
#ifdef OLR_UNIT_TEST
    bool m_failNextAdmission = false;
    QSemaphore* m_followerWaitingForTest = nullptr;
    QSemaphore* m_afterRejectedPublicationForTest = nullptr;
    QSemaphore* m_continueRejectedNotificationForTest = nullptr;
#endif
};

#endif // OLR_GPURECOVERYCOORDINATOR_H
