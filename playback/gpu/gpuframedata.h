#ifndef OLR_GPUFRAMEDATA_H
#define OLR_GPUFRAMEDATA_H

#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <QHash>
#include <QMutex>

#include <atomic>
#include <memory>

class GpuFrameData final : public IFrameData {
public:
    GpuFrameData(std::shared_ptr<GpuSurface> surface, std::shared_ptr<GpuRhiContext> rhi,
                 FramePixelFormat nativeFormat);
    ~GpuFrameData() override;

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    FramePixelFormat nativeFormat() const override { return m_nativeFormat; }

    int readToCpuCount() const { return m_readCount.load(std::memory_order_acquire); }

private:
    std::shared_ptr<GpuSurface> m_surface;
    std::shared_ptr<GpuRhiContext> m_rhi;
    FramePixelFormat m_nativeFormat = FramePixelFormat::Nv12;
    quint32 m_telemetryKey = 0;
    mutable std::atomic<int> m_readCount{0};
    mutable QMutex m_cacheMutex;
    mutable QHash<int, CpuPlanes> m_cpuCache;
};

FrameHandle makeGpuFrameHandle(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta);
qint64 gpuFrameReadToCpuCount();
void gpuResetFrameReadToCpuCount();

#endif // OLR_GPUFRAMEDATA_H
