#ifndef OLR_GPUSURFACELEASE_H
#define OLR_GPUSURFACELEASE_H

#include "playback/gpu/gpusurface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

class GpuDeviceLossMonitor;
struct AppleSurfaceBackingAccess;
struct D3D11ImportBackingAccess;
struct D3D11MediaFoundationBackingAccess;
struct VideoToolboxBackingAccess;

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

// A non-escapable view used only while GpuSyncReadScope is executing its callback.
// Ordinary callers can inspect safe metadata but cannot copy, move, or extract the
// backing. Named platform adapter authorities are defined only in their backend TUs.
class GpuReadLease final {
public:
    GpuReadLease(const GpuReadLease&) = delete;
    GpuReadLease& operator=(const GpuReadLease&) = delete;
    GpuReadLease(GpuReadLease&&) = delete;
    GpuReadLease& operator=(GpuReadLease&&) = delete;

    GpuSurfaceDesc desc() const { return m_surface ? m_surface->desc() : GpuSurfaceDesc{}; }
    bool valid() const { return m_surface && m_surface->isValid(); }

private:
    friend class GpuSyncReadScope;
    friend struct AppleSurfaceBackingAccess;
    friend struct D3D11ImportBackingAccess;
    friend struct D3D11MediaFoundationBackingAccess;
    friend struct VideoToolboxBackingAccess;

    explicit GpuReadLease(std::shared_ptr<GpuSurface> surface)
        : m_surface(surface.get()), m_owner(std::move(surface)) {}
    explicit GpuReadLease(GpuSurface* surface) : m_surface(surface) {}

    void* nativeHandleForBackend() const { return m_surface ? m_surface->nativeHandle() : nullptr; }

    GpuSurface* m_surface = nullptr;
    std::shared_ptr<GpuSurface> m_owner;
};

// Executes synchronous native access inside an inline callback. Callback return is
// the completion boundary; exceptions and early returns destroy the lease naturally.
// No std::function, allocation, or aliasing shared_ptr is introduced for raw owners.
class GpuSyncReadScope final {
public:
    GpuSyncReadScope() = default;
    GpuSyncReadScope(const GpuSyncReadScope&) = delete;
    GpuSyncReadScope& operator=(const GpuSyncReadScope&) = delete;

    template <typename Fn>
    decltype(auto) read(std::shared_ptr<GpuSurface> surface, Fn&& fn) const {
        GpuReadLease lease(std::move(surface));
        return std::invoke(std::forward<Fn>(fn), static_cast<const GpuReadLease&>(lease));
    }

    template <typename Surface, typename Fn,
              typename = std::enable_if_t<std::is_base_of_v<GpuSurface, Surface>>>
    decltype(auto) read(const std::shared_ptr<Surface>& surface, Fn&& fn) const {
        return read(std::static_pointer_cast<GpuSurface>(surface), std::forward<Fn>(fn));
    }

    template <typename Fn>
    decltype(auto) read(GpuSurface* surface, Fn&& fn) const {
        GpuReadLease lease(surface);
        return std::invoke(std::forward<Fn>(fn), static_cast<const GpuReadLease&>(lease));
    }
};

#endif // OLR_GPUSURFACELEASE_H
