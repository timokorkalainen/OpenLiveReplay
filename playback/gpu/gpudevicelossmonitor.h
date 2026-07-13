#ifndef OLR_GPUDEVICELOSSMONITOR_H
#define OLR_GPUDEVICELOSSMONITOR_H

#include "playback/gpu/gpusurfacelease.h" // DeadDeviceToken

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

class GpuRhiContext;
class WinGpuImportEdge;
#ifdef OLR_UNIT_TEST
struct GpuDeviceLossMonitorTestAuthority;
#endif

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
    uint64_t recordSubmissionFailure();

    // Carries the DeadDeviceToken from the driver-authoritative detection site to
    // the worker's recovery path (handleGpuDeviceLoss), which reads realLossToken()
    // to decide between the token-gated no-wait free (REAL loss) and the bounded-
    // wait drain (INJECTED loss, no token). A tokenless submission-failure epoch may
    // be upgraded by later driver-authoritative proof from the same device generation.
    // The injected-loss path calls only recordLoss(), so without that proof no token
    // is stored and the no-wait free can never run on a live device. A delayed/stale
    // mark is rejected unless its creation-time authority epoch still matches the
    // current rebuild epoch.
    std::optional<DeadDeviceToken> realLossToken() const;

    bool consumeLossEvent();
    // Invalidate authorities owned by the old device before constructing its
    // replacement. The loss latch remains set until clearForRebuild() commits a
    // successful rebuild.
    void beginRebuild();
    void clearForRebuild();
    void reset();

private:
    GpuDeviceLossMonitor() = default;
    friend class GpuRhiContext;
    friend class WinGpuImportEdge;
#ifdef OLR_UNIT_TEST
    friend struct GpuDeviceLossMonitorTestAuthority;
#endif

    uint64_t captureDeviceAuthorityEpoch() const;
    uint64_t publishRealDeviceLoss(DeadDeviceToken::Provenance provenance,
                                   uint64_t deviceAuthorityEpoch);

    std::atomic<bool> m_lost{false};
    std::atomic<uint64_t> m_lossCount{0};
    std::atomic<uint64_t> m_undrained{0};
    std::atomic<uint64_t> m_lossGeneration{0};
    mutable std::mutex m_epochMutex;
    uint64_t m_deviceAuthorityEpoch = 1;            // guarded by m_epochMutex
    bool m_rebuildInProgress = false;               // guarded by m_epochMutex
    std::optional<DeadDeviceToken> m_realLossToken; // guarded by m_epochMutex
};

#endif // OLR_GPUDEVICELOSSMONITOR_H
