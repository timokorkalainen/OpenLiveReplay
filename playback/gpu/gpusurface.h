#ifndef OLR_GPUSURFACE_H
#define OLR_GPUSURFACE_H

#include "playback/output/framepixelformat.h"

#include <QtGlobal>

#include <cstdint>

class GpuReadLease;

struct GpuSurfaceDesc {
    FramePixelFormat format = FramePixelFormat::Nv12;
    int width = 0;
    int height = 0;
    qint64 allocationBytes = 0;
};

// GPU-resident pixel surface behind the opaque GpuSurface forward declaration
// in framehandle.h. This shared header intentionally exposes no platform SDK
// types; platform import code downcasts nativeHandle() inside .mm/.cpp files.
class GpuSurface {
public:
    // Defaulted inline so the (abstract) vtable and typeinfo are weak-emitted in
    // every translation unit that uses GpuSurface. Record-side code (the
    // VideoToolbox encoder's encodeSurface) references it even in GPU-off builds
    // that do not link the playback GPU library, so there is no single anchor TU.
    virtual ~GpuSurface() = default;

    virtual GpuSurfaceDesc desc() const = 0;
    virtual bool isValid() const = 0;

    // Presentability probe: is this surface backed by a real native handle? A
    // capability-free bool that leaks no pointer, so sinks can gate on GPU
    // backing without a lease (e.g. DeckLink presentability, gpusurfacelease.h).
    bool hasNativeBacking() const { return nativeHandle() != nullptr; }

    // The retention watermark is deliberately public: it is a monotonic-max write
    // and a read, neither of which can cause a use-after-free on its own. The
    // frame-retire queue and the readback retainer legitimately read/stamp it
    // without a lease. Only nativeHandle() — the raw GPU handle, the one capability
    // that CAN be misused into a UAF — is guarded (see gpusurfacelease.h).
    virtual void retainUntilFenceRetired(uint64_t fenceValue) { (void) fenceValue; }
    virtual uint64_t pendingFenceValue() const { return 0; }

protected:
    // THE enforcement point (Challenge 3): the raw GPU handle is reachable ONLY
    // through a GpuReadLease, which is obtainable only from a scope whose
    // destruction discharges the retire obligation. A new op site that tries
    // surface->nativeHandle() directly no longer compiles (negative-compile test).
    friend class GpuReadLease;
    // IOSurfaceRef on Apple, ID3D11Texture2D* on Windows.
    virtual void* nativeHandle() const = 0;
};

#endif // OLR_GPUSURFACE_H
