#ifndef OLR_GPURECOVERYCOORDINATOR_H
#define OLR_GPURECOVERYCOORDINATOR_H

#include <QtGlobal>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

enum class GpuValidatedLossStatus : uint8_t { Rejected, Completed };

struct GpuValidatedLossResult {
    GpuValidatedLossStatus status = GpuValidatedLossStatus::Rejected;
    qsizetype abandoned = 0;
    bool coordinatorLeader = false;
};

class GpuRecoveryCoordinator final {
public:
    static GpuRecoveryCoordinator& instance();
    GpuValidatedLossResult coordinate(uint64_t lossGeneration, uint64_t proofRevision,
                                      const std::function<GpuValidatedLossResult()>& leaderWork);
    void retireGenerationsThrough(uint64_t lossGeneration);
#ifdef OLR_UNIT_TEST
    void resetForTest();
    std::size_t cachedRecoveryCountForTest();
#endif

private:
    GpuRecoveryCoordinator() = default;

    struct RecoveryState {
        std::condition_variable finished;
        bool inProgress = false;
        bool completed = false;
        GpuValidatedLossResult result;
    };

    std::mutex m_mutex;
    std::map<std::pair<uint64_t, uint64_t>, std::shared_ptr<RecoveryState>> m_recoveries;
    uint64_t m_retiredGeneration = 0;
};

#endif // OLR_GPURECOVERYCOORDINATOR_H
