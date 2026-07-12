#ifndef OLR_GPUFENCE_H
#define OLR_GPUFENCE_H

#include "playback/gpu/gpusurface.h"

#include <cstdint>
#include <memory>

class GpuFence {
public:
    virtual ~GpuFence();

    virtual uint64_t signal() = 0;
    // timeoutMs < 0 waits indefinitely.
    virtual bool wait(uint64_t value, int timeoutMs) = 0;
    virtual uint64_t completedValue() const = 0;
    bool isCompatibleWith(const GpuSurface& surface) const {
        return isCompatibleWithNativeHandle(surface.nativeHandle());
    }

    static std::shared_ptr<GpuFence> create();

protected:
    virtual bool isCompatibleWithNativeHandle(void* nativeHandle) const {
        return nativeHandle == nullptr;
    }
};

#ifdef __APPLE__
std::shared_ptr<GpuFence> makeMetalGpuFence(void* metalCommandQueue);
#endif

#ifdef _WIN32
std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device);
#endif

#endif // OLR_GPUFENCE_H
