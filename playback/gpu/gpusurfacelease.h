#ifndef OLR_GPUSURFACELEASE_H
#define OLR_GPUSURFACELEASE_H

#include "playback/gpu/gpusurface.h"

#include <cstdint>
#include <memory>
#include <type_traits>

class GpuDeviceLossMonitor;

// Proof that a driver-authoritative observation declared the active GPU device dead.
// Only GpuDeviceLossMonitor can construct it, and only backend-local authority types
// can ask the monitor to publish one. Injected loss therefore remains tokenless.
class DeadDeviceToken {
public:
    enum class Provenance : uint8_t {
        DxgiDeviceRemovedReason,
        RhiFrameOpDeviceLost,
    };

    Provenance provenance() const { return m_provenance; }
    uint64_t observedGeneration() const { return m_generation; }

private:
    friend class GpuDeviceLossMonitor;
    DeadDeviceToken(Provenance provenance, uint64_t generation)
        : m_provenance(provenance), m_generation(generation) {}

    Provenance m_provenance;
    uint64_t m_generation;
};

// A self-contained synchronous snapshot. It stores no GpuSurface pointer: metadata
// is copied and any native backing is independently retained before read() returns.
class GpuReadLease final {
public:
    GpuReadLease(const GpuReadLease&) = delete;
    GpuReadLease& operator=(const GpuReadLease&) = delete;
    GpuReadLease(GpuReadLease&&) = delete;
    GpuReadLease& operator=(GpuReadLease&&) = delete;

    GpuSurfaceDesc desc() const { return m_desc; }
    bool valid() const { return m_valid; }
    std::shared_ptr<void> retainNativeHandle() const { return m_nativeHandle; }
    uint32_t nativeSubresource() const { return m_nativeSubresource; }

private:
    friend class GpuSyncReadScope;

    explicit GpuReadLease(const std::shared_ptr<GpuSurface>& surface)
        : GpuReadLease(surface.get()) {}
    explicit GpuReadLease(GpuSurface* surface)
        : m_desc(surface ? surface->desc() : GpuSurfaceDesc{}),
          m_valid(surface && surface->isValid()),
          m_nativeHandle(surface ? surface->retainNativeHandle() : std::shared_ptr<void>{}),
          m_nativeSubresource(surface ? surface->nativeSubresource() : 0) {}

    GpuSurfaceDesc m_desc;
    bool m_valid = false;
    std::shared_ptr<void> m_nativeHandle;
    uint32_t m_nativeSubresource = 0;
};

// Captures a lease snapshot synchronously. The returned value contains no borrowed
// pointer, including for the raw-surface overload used by the encoder.
class GpuSyncReadScope final {
public:
    GpuSyncReadScope() = default;
    GpuSyncReadScope(const GpuSyncReadScope&) = delete;
    GpuSyncReadScope& operator=(const GpuSyncReadScope&) = delete;

    GpuReadLease read(const std::shared_ptr<GpuSurface>& surface) const {
        return GpuReadLease(surface);
    }

    template <typename Surface, typename = std::enable_if_t<std::is_base_of_v<GpuSurface, Surface>>>
    GpuReadLease read(const std::shared_ptr<Surface>& surface) const {
        return GpuReadLease(std::static_pointer_cast<GpuSurface>(surface));
    }

    GpuReadLease read(GpuSurface* surface) const { return GpuReadLease(surface); }
};

#endif // OLR_GPUSURFACELEASE_H
