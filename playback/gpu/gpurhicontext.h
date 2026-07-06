#ifndef OLR_GPURHICONTEXT_H
#define OLR_GPURHICONTEXT_H

#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"

#include <functional>
#include <memory>

class GpuFence;
class QRhi;

// Owns the platform QRhi on a dedicated render thread. QRhi and imported GPU
// textures are thread-affine, so all RHI work funnels through this context.
class GpuRhiContext {
public:
    static std::shared_ptr<GpuRhiContext> create();
    static std::shared_ptr<GpuRhiContext> createNullForTest();
    static std::shared_ptr<GpuRhiContext> createWarpForTest();
#ifdef OLR_UNIT_TEST
    static std::shared_ptr<GpuRhiContext> createInvalidForTest();
    int rhiReadbackCountForTest() const;
#endif
    ~GpuRhiContext();

    GpuRhiContext(const GpuRhiContext&) = delete;
    GpuRhiContext& operator=(const GpuRhiContext&) = delete;

    bool isValid() const;
    bool isNullBackend() const;
    bool isGpuBacked() const { return isValid() && !isNullBackend(); }

    bool invokeOnRenderThread(const std::function<void(QRhi*)>& job) const;
    CpuPlanes importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                FramePixelFormat target);
    // Run a UIKit/CAMetalLayer-touching present block on the platform's present
    // thread. iOS marshals to the main queue; macOS and stubs run inline.
    void presentOnMainThread(const std::function<void()>& block);
    bool deviceLost() const;
    void injectDeviceLostForTest();
    std::shared_ptr<GpuFence> createFence() const;

private:
    class Impl;

    explicit GpuRhiContext(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

#endif // OLR_GPURHICONTEXT_H
