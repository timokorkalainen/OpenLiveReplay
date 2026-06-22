#ifndef OLR_GPUCOMPOSITOR_H
#define OLR_GPUCOMPOSITOR_H

#include "playback/output/framehandle.h"

#include <QList>
#include <QVector>

#include <memory>

class GpuRhiContext;
class GpuSurface;
struct MultiviewComposite;

// RHI grid + PGM-select compositor. The CPU Yuv420pCompositor remains the
// correctness oracle and fallback; a null FrameHandle from this type means the
// caller must degrade to the CPU path.
class GpuCompositor {
public:
    enum class ScaleQuality { NearestCompat, Bilinear, Lanczos };

    static std::shared_ptr<GpuCompositor> create(std::shared_ptr<GpuRhiContext> rhi);
    static std::shared_ptr<GpuSurface>
    uploadFrameToNv12SurfaceForTest(const FrameHandle& frame,
                                    const std::shared_ptr<GpuRhiContext>& rhi);

    ~GpuCompositor();

    GpuCompositor(const GpuCompositor&) = delete;
    GpuCompositor& operator=(const GpuCompositor&) = delete;

    bool isValid() const;

    FrameHandle composeGrid(const QList<FrameHandle>& frames, int width, int height,
                            ColorMetadata color, ScaleQuality quality) const;
    FrameHandle composeGridMemoized(const QList<FrameHandle>& frames, int width, int height,
                                    ColorMetadata color, ScaleQuality quality,
                                    const QVector<qint64>& sourceKeys,
                                    MultiviewComposite* memo) const;
    CpuPlanes composeGridToCpu(const QList<FrameHandle>& frames, int width, int height,
                               ColorMetadata color, ScaleQuality quality) const;
    FrameHandle composePgm(const FrameHandle& source, int width, int height, ColorMetadata color,
                           ScaleQuality quality) const;

private:
    class Impl;

    explicit GpuCompositor(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

#endif // OLR_GPUCOMPOSITOR_H
