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
    enum class Phase : uint8_t {
        Ready,
        Reading,
        Completed,
        Violated,
    };

    Phase phase = Phase::Ready;
};

class GpuReadLease;

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
    ~GpuReadLease() {
        if (m_registration && *m_registration == this) *m_registration = nullptr;
    }

    GpuSurfaceDesc desc() const { return m_desc; }
    bool valid() const { return m_valid; }
    void* nativeHandle() const {
        // Checked builds stop at the violated contract. Release builds diagnose once and
        // return null so noexcept driver adapters take their existing invalid-handle path;
        // throwing or terminating here would turn a contained capability bug into an
        // unconditional broadcast-process outage.
        const bool active = m_accessAuthorized;
        Q_ASSERT_X(active, "GpuReadLease::nativeHandle",
                   "native handle access requires an active GpuSyncReadScope");
#ifdef QT_NO_DEBUG
        if (!active) {
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set(std::memory_order_relaxed))
                qWarning("GpuReadLease rejected native handle access outside its read scope");
        }
#endif
        return active ? m_nativeHandle : nullptr;
    }
    uint32_t nativeSubresource() const { return m_nativeSubresource; }

private:
    friend class GpuSyncReadScope;

    GpuReadLease(const std::shared_ptr<GpuSurface>& surface, GpuReadLease** registration,
                 bool accessAuthorized)
        : m_desc(accessAuthorized && surface ? surface->desc() : GpuSurfaceDesc{}),
          m_valid(accessAuthorized && surface && surface->isValid()),
          m_surfaceOwner(accessAuthorized ? surface : std::shared_ptr<GpuSurface>{}),
          m_nativeHandle(accessAuthorized && surface ? surface->nativeHandle() : nullptr),
          m_nativeSubresource(accessAuthorized && surface ? surface->nativeSubresource() : 0),
          m_accessAuthorized(accessAuthorized),
          m_registration(accessAuthorized ? registration : nullptr) {
        if (m_registration) *m_registration = this;
    }
    GpuReadLease(GpuSurface* surface, GpuReadLease** registration, bool accessAuthorized)
        : m_desc(accessAuthorized && surface ? surface->desc() : GpuSurfaceDesc{}),
          m_valid(accessAuthorized && surface && surface->isValid()),
          m_nativeOwner(accessAuthorized && surface ? surface->retainNativeHandle()
                                                    : GpuOwnedNativeHandle{}),
          m_nativeHandle(m_nativeOwner.get()),
          m_nativeSubresource(accessAuthorized && surface ? surface->nativeSubresource() : 0),
          m_accessAuthorized(accessAuthorized),
          m_registration(accessAuthorized ? registration : nullptr) {
        if (m_registration) *m_registration = this;
    }

    void invalidateAccess() noexcept {
        m_accessAuthorized = false;
        m_registration = nullptr;
    }

    GpuSurfaceDesc m_desc;
    bool m_valid = false;
    std::shared_ptr<GpuSurface> m_surfaceOwner;
    GpuOwnedNativeHandle m_nativeOwner;
    void* m_nativeHandle = nullptr;
    uint32_t m_nativeSubresource = 0;
    bool m_accessAuthorized = false;
    GpuReadLease** m_registration = nullptr;
};

// Captures a lease snapshot synchronously. Every raw native pointer is backed by an
// owner stored in the returned lease, including the raw-surface encoder overload. The
// scope and its one non-movable lease register each other without allocation: whichever
// is destroyed first clears the registration, so access invalidation never follows a
// dangling scope pointer.
class GpuSyncReadScope final {
public:
    GpuSyncReadScope() = default;
    GpuSyncReadScope(const GpuSyncReadScope&) = delete;
    GpuSyncReadScope& operator=(const GpuSyncReadScope&) = delete;
    GpuSyncReadScope(GpuSyncReadScope&&) = delete;
    GpuSyncReadScope& operator=(GpuSyncReadScope&&) = delete;
    ~GpuSyncReadScope() {
        invalidateActiveLease();
        if (m_state.phase != GpuSyncReadState::Phase::Reading) return;
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
        const bool accessAuthorized = beginRead();
        return GpuReadLease(surface, &m_activeLease, accessAuthorized);
    }

    template <typename Surface, typename = std::enable_if_t<std::is_base_of_v<GpuSurface, Surface>>>
    GpuReadLease read(const std::shared_ptr<Surface>& surface) {
        return read(std::static_pointer_cast<GpuSurface>(surface));
    }

    GpuReadLease read(GpuSurface* surface) {
        const bool accessAuthorized = beginRead();
        return GpuReadLease(surface, &m_activeLease, accessAuthorized);
    }

    void complete() noexcept {
        if (m_state.phase != GpuSyncReadState::Phase::Reading) {
            invalidateActiveLease();
            m_state.phase = GpuSyncReadState::Phase::Violated;
            Q_ASSERT_X(false, "GpuSyncReadScope::complete",
                       "complete() requires exactly one active read");
#ifdef QT_NO_DEBUG
            reportViolation("GpuSyncReadScope rejected complete() without one active read");
#endif
            return;
        }
        invalidateActiveLease();
        m_state.phase = GpuSyncReadState::Phase::Completed;
    }

    template <typename Surface, typename Fn>
    void withRead(const std::shared_ptr<Surface>& surface, Fn&& fn) {
        using Result = std::invoke_result_t<Fn&&, const GpuReadLease&>;
        static_assert(std::is_same_v<Result, void>,
                      "GpuSyncReadScope::withRead callback must return void");
        const GpuReadLease lease = read(surface);
        if (!lease.m_accessAuthorized) return;
        struct CompleteOnExit {
            GpuSyncReadScope* scope;
            ~CompleteOnExit() { scope->complete(); }
        } completeOnExit{this};
        std::invoke(std::forward<Fn>(fn), lease);
    }

    template <typename Surface, typename Fn,
              typename = std::enable_if_t<std::is_base_of_v<GpuSurface, Surface>>>
    void withRead(Surface* surface, Fn&& fn) {
        using Result = std::invoke_result_t<Fn&&, const GpuReadLease&>;
        static_assert(std::is_same_v<Result, void>,
                      "GpuSyncReadScope::withRead callback must return void");
        const GpuReadLease lease = read(static_cast<GpuSurface*>(surface));
        if (!lease.m_accessAuthorized) return;
        struct CompleteOnExit {
            GpuSyncReadScope* scope;
            ~CompleteOnExit() { scope->complete(); }
        } completeOnExit{this};
        std::invoke(std::forward<Fn>(fn), lease);
    }

private:
    bool beginRead() noexcept {
        if (m_state.phase != GpuSyncReadState::Phase::Ready) {
            invalidateActiveLease();
            m_state.phase = GpuSyncReadState::Phase::Violated;
            Q_ASSERT_X(false, "GpuSyncReadScope::read",
                       "a synchronous read scope permits exactly one acquisition");
#ifdef QT_NO_DEBUG
            reportViolation("GpuSyncReadScope rejected a repeated or post-complete acquisition");
#endif
            return false;
        }
        m_state.phase = GpuSyncReadState::Phase::Reading;
        return true;
    }

    void invalidateActiveLease() noexcept {
        if (!m_activeLease) return;
        m_activeLease->invalidateAccess();
        m_activeLease = nullptr;
    }

#ifdef QT_NO_DEBUG
    static void reportViolation(const char* message) noexcept {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed)) qWarning("%s", message);
    }
#endif

    GpuSyncReadState m_state;
    GpuReadLease* m_activeLease = nullptr;
};

#endif // OLR_GPUSURFACELEASE_H
