#ifndef OLR_GPUDEVICELOSSMONITOR_H
#define OLR_GPUDEVICELOSSMONITOR_H

#include <atomic>
#include <cstdint>
#include <mutex>

// Process-wide GPU device-loss latch. A loss is a hard-down for a live tool:
// recordLoss() bumps GpuGenerationCounter so every FrameHandle stamped under
// the dead device is stale, and consumeLossEvent() drains telemetry events.
class GpuDeviceLossMonitor {
public:
    static GpuDeviceLossMonitor& instance();

    bool isLost() const;
    uint64_t lossCount() const;

    // Idempotent while the latch is already lost. A fresh loss epoch begins only
    // after clearForRebuild() has cleared the latch following a successful rebuild.
    uint64_t recordLoss();

    bool consumeLossEvent();
    void clearForRebuild();
    void reset();

private:
    GpuDeviceLossMonitor() = default;

    std::atomic<bool> m_lost{false};
    std::atomic<uint64_t> m_lossCount{0};
    std::atomic<uint64_t> m_undrained{0};
    std::atomic<uint64_t> m_lossGeneration{0};
    std::mutex m_epochMutex;
};

#endif // OLR_GPUDEVICELOSSMONITOR_H
