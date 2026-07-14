#ifndef OLR_GPUSURFACELEASE_H
#define OLR_GPUSURFACELEASE_H

#include "playback/gpu/gpusurface.h"

#include <QtLogging>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

class GpuDeviceLossMonitor;

struct GpuSyncReadState {
    bool active = true;
    bool read = false;
};

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
    uint64_t authorityEpoch() const { return m_authorityEpoch; }
    uintptr_t deviceDomainId() const { return m_deviceDomainId; }

private:
    friend class GpuDeviceLossMonitor;
    DeadDeviceToken(Provenance provenance, uint64_t generation, uintptr_t deviceDomainId,
                    uint64_t authorityEpoch)
        : m_provenance(provenance), m_generation(generation), m_deviceDomainId(deviceDomainId),
          m_authorityEpoch(authorityEpoch) {}

    Provenance m_provenance;
    uint64_t m_generation;
    uintptr_t m_deviceDomainId = 0;
    uint64_t m_authorityEpoch = 0;
};

// A self-contained synchronous snapshot. Metadata is copied and the native backing
// stays alive through either the caller's shared surface owner or a move-only native
// reference acquired for the raw-pointer encoder overload.
class GpuReadLease final {
public:
    GpuReadLease(const GpuReadLease&) = delete;
    GpuReadLease& operator=(const GpuReadLease&) = delete;
    GpuReadLease(GpuReadLease&&) = delete;
    GpuReadLease& operator=(GpuReadLease&&) = delete;

    GpuSurfaceDesc desc() const { return m_desc; }
    bool valid() const { return m_valid; }
    void* nativeHandle() const {
        Q_ASSERT_X(m_state && m_state->active, "GpuReadLease::nativeHandle",
                   "native handle access requires an active GpuSyncReadScope");
        return m_nativeHandle;
    }
    uint32_t nativeSubresource() const { return m_nativeSubresource; }

private:
    friend class GpuSyncReadScope;

    GpuReadLease(const std::shared_ptr<GpuSurface>& surface, GpuSyncReadState* state)
        : m_state(state), m_desc(surface ? surface->desc() : GpuSurfaceDesc{}),
          m_valid(surface && surface->isValid()), m_surfaceOwner(surface),
          m_nativeHandle(surface ? surface->nativeHandle() : nullptr),
          m_nativeSubresource(surface ? surface->nativeSubresource() : 0) {}
    GpuReadLease(GpuSurface* surface, GpuSyncReadState* state)
        : m_state(state), m_desc(surface ? surface->desc() : GpuSurfaceDesc{}),
          m_valid(surface && surface->isValid()),
          m_nativeOwner(surface ? surface->retainNativeHandle() : GpuOwnedNativeHandle{}),
          m_nativeHandle(m_nativeOwner.get()),
          m_nativeSubresource(surface ? surface->nativeSubresource() : 0) {}

    GpuSyncReadState* m_state = nullptr;
    GpuSurfaceDesc m_desc;
    bool m_valid = false;
    std::shared_ptr<GpuSurface> m_surfaceOwner;
    GpuOwnedNativeHandle m_nativeOwner;
    void* m_nativeHandle = nullptr;
    uint32_t m_nativeSubresource = 0;
};

// Captures a lease snapshot synchronously. Every raw native pointer is backed by an
// owner stored in the returned lease, including the raw-surface encoder overload.
class GpuSyncReadScope final {
public:
    GpuSyncReadScope() = default;
    GpuSyncReadScope(const GpuSyncReadScope&) = delete;
    GpuSyncReadScope& operator=(const GpuSyncReadScope&) = delete;
    ~GpuSyncReadScope() {
        if (!m_state.read || !m_state.active) return;
#ifndef QT_NO_DEBUG
        Q_ASSERT_X(false, "GpuSyncReadScope::~GpuSyncReadScope",
                   "complete() must be called after synchronous native handle access");
#else
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed))
            qWarning("GpuSyncReadScope destroyed without complete() after a read");
#endif
    }

    GpuReadLease read(const std::shared_ptr<GpuSurface>& surface) {
        beginRead();
        return GpuReadLease(surface, &m_state);
    }

    template <typename Surface, typename = std::enable_if_t<std::is_base_of_v<GpuSurface, Surface>>>
    GpuReadLease read(const std::shared_ptr<Surface>& surface) {
        return read(std::static_pointer_cast<GpuSurface>(surface));
    }

    GpuReadLease read(GpuSurface* surface) {
        beginRead();
        return GpuReadLease(surface, &m_state);
    }

    void complete() noexcept { m_state.active = false; }

    template <typename Surface, typename Fn>
    decltype(auto) withRead(const std::shared_ptr<Surface>& surface, Fn&& fn) {
        const GpuReadLease lease = read(surface);
        struct CompleteOnExit {
            GpuSyncReadScope* scope;
            ~CompleteOnExit() { scope->complete(); }
        } completeOnExit{this};
        return std::invoke(std::forward<Fn>(fn), lease);
    }

    template <typename Surface, typename Fn,
              typename = std::enable_if_t<std::is_base_of_v<GpuSurface, Surface>>>
    decltype(auto) withRead(Surface* surface, Fn&& fn) {
        const GpuReadLease lease = read(static_cast<GpuSurface*>(surface));
        struct CompleteOnExit {
            GpuSyncReadScope* scope;
            ~CompleteOnExit() { scope->complete(); }
        } completeOnExit{this};
        return std::invoke(std::forward<Fn>(fn), lease);
    }

private:
    void beginRead() {
        Q_ASSERT_X(m_state.active, "GpuSyncReadScope::read",
                   "cannot acquire a lease after complete()");
        m_state.read = true;
    }

    GpuSyncReadState m_state;
};

#endif // OLR_GPUSURFACELEASE_H
