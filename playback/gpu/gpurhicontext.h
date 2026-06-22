#ifndef OLR_GPURHICONTEXT_H
#define OLR_GPURHICONTEXT_H

#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <memory>

class GpuFence;

// Owns the platform QRhi on a dedicated render thread. QRhi and imported GPU
// textures are thread-affine, so all RHI work funnels through this context.
class GpuRhiContext {
public:
    static std::shared_ptr<GpuRhiContext> create();
    ~GpuRhiContext();

    GpuRhiContext(const GpuRhiContext&) = delete;
    GpuRhiContext& operator=(const GpuRhiContext&) = delete;

    bool isValid() const;

    CpuPlanes importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                FramePixelFormat target);
    std::shared_ptr<GpuFence> createFence() const;

private:
    class Impl;

    explicit GpuRhiContext(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

#endif // OLR_GPURHICONTEXT_H
