#ifndef OLR_GPURECOVERYCOORDINATOR_H
#define OLR_GPURECOVERYCOORDINATOR_H

#include <QtGlobal>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

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
    bool completed(uint64_t lossGeneration, uint64_t proofRevision);
#ifdef OLR_UNIT_TEST
    void resetForTest();
#endif

private:
    GpuRecoveryCoordinator() = default;

    std::mutex m_mutex;
    std::condition_variable m_finished;
    uint64_t m_activeGeneration = 0;
    uint64_t m_activeRevision = 0;
    uint64_t m_completedGeneration = 0;
    uint64_t m_completedRevision = 0;
    bool m_inProgress = false;
    GpuValidatedLossResult m_completedResult;
};

#endif // OLR_GPURECOVERYCOORDINATOR_H
