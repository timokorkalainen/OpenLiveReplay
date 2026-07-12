#ifndef OLR_GPUDEVICELOSSMONITOR_H
#define OLR_GPUDEVICELOSSMONITOR_H

#include "playback/gpu/gpusurfacelease.h" // DeadDeviceToken

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

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

    // Carries the DeadDeviceToken from the driver-authoritative detection site to
    // the worker's recovery path (handleGpuDeviceLoss), which reads realLossToken()
    // to decide between the token-gated no-wait free (REAL loss) and the bounded-
    // wait drain (INJECTED loss, no token). First writer within an epoch wins,
    // mirroring recordLoss(). The injected-loss path calls only recordLoss(), so no
    // token is ever stored for it — the no-wait free can never run on a live device.
    void markRealDeviceLoss(const DeadDeviceToken& token);
    std::optional<DeadDeviceToken> realLossToken() const;

    bool consumeLossEvent();
    void clearForRebuild();
    void reset();

private:
    GpuDeviceLossMonitor() = default;

    std::atomic<bool> m_lost{false};
    std::atomic<uint64_t> m_lossCount{0};
    std::atomic<uint64_t> m_undrained{0};
    std::atomic<uint64_t> m_lossGeneration{0};
    mutable std::mutex m_epochMutex;
    std::optional<DeadDeviceToken> m_realLossToken; // guarded by m_epochMutex
};

#endif // OLR_GPUDEVICELOSSMONITOR_H
