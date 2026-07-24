#ifndef OLR_GPUFRAMEDATA_H
#define OLR_GPUFRAMEDATA_H

#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpuframereadbacktelemetry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <QHash>
#include <QMutex>

#include <atomic>
#include <memory>

class GpuFence;

class GpuFrameData final : public IFrameData {
public:
    GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                 FramePixelFormat nativeFormat, ColorMetadata color = {},
                 std::shared_ptr<GpuFence> renderFence = nullptr, GpuBudgetCharge budgetCharge = {},
                 uint64_t gpuGeneration = 0, uint64_t renderFenceValue = 0);
    ~GpuFrameData() override;

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    CpuPlanes cachedCpuPlanes(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_renderFence; }
    GpuFrameSynchronization gpuSynchronization() const override {
        if (m_renderFenceValue == 0) return {nullptr, 0, true};
        return {m_renderFence, m_renderFenceValue, true};
    }
    FramePixelFormat nativeFormat() const override { return m_nativeFormat; }
    std::shared_ptr<GpuSurface> surfacePtr() const { return m_surface; }
    bool waitForPendingFence(int timeoutMs) const;

    int readToCpuCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    struct CpuCacheEntry;

    std::shared_ptr<GpuSurface> m_surface;
    std::shared_ptr<GpuRhiContext> m_rhi;
    std::shared_ptr<GpuFence> m_renderFence;
    uint64_t m_renderFenceValue = 0;
    GpuBudgetCharge m_budgetCharge;
    FramePixelFormat m_nativeFormat = FramePixelFormat::Nv12;
    ColorMetadata m_color;
    uint64_t m_gpuGeneration = 0;
    mutable std::atomic<int> m_readCount{0};
    mutable QMutex m_cacheMutex;
    mutable QHash<int, std::shared_ptr<const CpuCacheEntry>> m_cpuCache;
};

// Performs the real readback only after exact-surface retirement ownership is
// prepared. Keep GpuOpScope's submission representation private to this helper
// so callers cannot discard the typed outcome before failure accounting.
GpuReadbackResult submitGpuReadback(const std::shared_ptr<GpuRhiContext>& rhi,
                                    const std::shared_ptr<GpuSurface>& surface,
                                    FramePixelFormat target) noexcept;

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta);
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence);
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge charge);
FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence, uint64_t renderFenceValue,
                               GpuBudgetCharge charge);

#endif // OLR_GPUFRAMEDATA_H
