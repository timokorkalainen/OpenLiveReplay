#ifndef OLR_GPUSURFACELEASE_H
#define OLR_GPUSURFACELEASE_H

// Type-state protocol that makes "the raw GPU handle is reachable only inside a
// bounded read scope" UNREPRESENTABLE to break: GpuSurface::nativeHandle() is
// protected (gpusurface.h) with GpuReadLease as its sole friend, and the ONLY route
// to a lease is a scope. A new op site that writes surface->nativeHandle() directly
// no longer compiles (negative-compile test, tests/gpu/negcompile).
//
// Shared header, like gpusurface.h: exposes no platform SDK types and is HEADER-ONLY.
// It is included by BOTH the playback GPU library AND the record-side VideoToolbox
// encoder, which does NOT link the playback GPU library (see gpusurface.h:22-25), so
// the encoder can lease a handle with no link dependency.
//
// Scope of this file (Challenge 3, part 1): the handle-access gate + the device-loss
// provenance token. Every production handle access is SYNCHRONOUS (CPU readback
// returns real pixels; the compositor wraps the IOSurface synchronously; the encoder
// submit hands VideoToolbox its own retained buffer), so GpuSyncReadScope covers them
// all and the existing render-keep-alive retains (gpuRetainSurfaceUntilFenceRetired,
// public) are untouched. A fenced RAII scope that also makes the render keep-alive a
// compile obligation is a natural follow-up now that these primitives exist.

#include "playback/gpu/gpusurface.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

// Provenance-bound proof that the device is REALLY dead. Constructible ONLY by the
// two driver-authoritative observation sites (the friend mints, defined only in
// gpurhicontext_win.cpp / gpurhicontext_apple.mm). The test-injection path cannot
// mint one, so the no-wait free (gpuAbandonAllReadbackRetains) can never run on a
// live device. Freely copyable once minted so it can be carried through the loss
// latch (GpuDeviceLossMonitor) to the recovery site.
class DeadDeviceToken {
public:
    enum class Provenance : uint8_t {
        DxgiDeviceRemovedReason, // Windows: FAILED(GetDeviceRemovedReason())
        RhiFrameOpDeviceLost,    // Apple/RHI: FrameOpDeviceLost / rhi->isDeviceLost()
    };
    Provenance provenance() const { return m_p; }
    uint64_t observedGeneration() const { return m_gen; }

private:
    DeadDeviceToken(Provenance p, uint64_t gen) : m_p(p), m_gen(gen) {}
    // Defined ONLY at the driver-authoritative detection branch on each backend.
    friend DeadDeviceToken mintDeadDeviceTokenFromDxgi(long failedHr, uint64_t gen);
    friend DeadDeviceToken mintDeadDeviceTokenFromFrameOp(uint64_t gen);
    Provenance m_p;
    uint64_t m_gen;
};

// Namespace-scope declarations of the two mints (a friend declaration alone is not
// found by ordinary lookup). DEFINED only in the platform detection TUs:
// mintDeadDeviceTokenFromDxgi in gpurhicontext_win.cpp, mintDeadDeviceTokenFromFrameOp
// in gpurhicontext_apple.mm. No other TU can construct a DeadDeviceToken.
DeadDeviceToken mintDeadDeviceTokenFromDxgi(long failedHr, uint64_t gen);
DeadDeviceToken mintDeadDeviceTokenFromFrameOp(uint64_t gen);

// Move-only view of a surface's native handle INSIDE one read scope. The ONLY route
// to nativeHandle() now that GpuSurface makes it protected. A friend of GpuSurface;
// header-only so record-side code uses it without linking playback/gpu.
class GpuReadLease {
public:
    GpuReadLease(GpuReadLease&&) noexcept = default;
    GpuReadLease& operator=(GpuReadLease&&) noexcept = default;
    GpuReadLease(const GpuReadLease&) = delete;
    GpuReadLease& operator=(const GpuReadLease&) = delete;

    void* nativeHandle() const { return m_surface ? m_surface->nativeHandle() : nullptr; }
    GpuSurfaceDesc desc() const { return m_surface ? m_surface->desc() : GpuSurfaceDesc{}; }
    bool valid() const { return m_surface && m_surface->isValid(); }

private:
    friend class GpuSyncReadScope;
    explicit GpuReadLease(std::shared_ptr<GpuSurface> s) : m_surface(std::move(s)) {}
    std::shared_ptr<GpuSurface> m_surface;
};

// SYNCHRONOUS read scope, typed. For paths whose GPU read provably completes before
// control returns: the record-side VideoToolbox encodeSurface (submit accepts the
// IOSurface), the Apple CPU readbacks, and the compositor IOSurface wrapping. No
// fence — the read is done when the scope ends. The destructor asserts complete() was
// called for any leased surface (the wrapped helper calls it after the synchronous op
// returns), so "the read finished before the surface was released" is a checked
// obligation, not an unwritten assumption.
class GpuSyncReadScope {
public:
    GpuSyncReadScope() = default;
    GpuSyncReadScope(const GpuSyncReadScope&) = delete;
    GpuSyncReadScope& operator=(const GpuSyncReadScope&) = delete;
    ~GpuSyncReadScope() {
        assert((m_leased == 0 || m_completed) &&
               "GpuSyncReadScope: complete() not called before a leased surface was released");
    }

    GpuReadLease read(std::shared_ptr<GpuSurface> s) {
        ++m_leased;
        return GpuReadLease(std::move(s));
    }
    // Non-owning overload for callers that hold a raw GpuSurface* (the record-side
    // encoder). Safe because the read is synchronous and the caller owns the surface
    // for the scope's lifetime; the lease keeps a non-owning alias only.
    GpuReadLease read(GpuSurface* s) {
        ++m_leased;
        return GpuReadLease(std::shared_ptr<GpuSurface>(s, [](GpuSurface*) {}));
    }
    // Called by the wrapped synchronous helper once the GPU read has provably
    // completed (download returned / encode submission accepted the surface).
    void complete() { m_completed = true; }

private:
    int m_leased = 0;
    bool m_completed = false;
};

#endif // OLR_GPUSURFACELEASE_H
