#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurfacelease.h"

#include <array>
#include <memory>

class SubmissionPassSurface final : public GpuSurface {
public:
    GpuSurfaceDesc desc() const override { return {}; }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override { return {1, 1}; }

protected:
    void* nativeHandle() const override { return reinterpret_cast<void*>(0x1); }
};

struct SubmissionPassAdapter {
    GpuSubmitOutcome operator()(const GpuScopedNativeView<1>& view) noexcept {
        return view[0].nativeHandle() ? GpuSubmitOutcome::Submitted
                                      : GpuSubmitOutcome::NotSubmitted;
    }
};

GpuSubmissionResult allowedOwnedSubmission(std::shared_ptr<GpuFence> fence,
                                           std::shared_ptr<SubmissionPassSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    SubmissionPassAdapter adapter;
    return operation.submit(adapter,
                            GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{surface}));
}

bool allowedScopedSubmissionHandleRead(const std::shared_ptr<SubmissionPassSurface>& surface) {
    bool hasHandle = false;
    GpuSyncReadScope scope;
    scope.withRead(surface,
                   [&](const GpuReadLease& lease) { hasHandle = lease.nativeHandle() != nullptr; });
    return hasHandle;
}

GpuValidatedLossResult allowedValidatedAbandon(GpuRetireRegistry& registry) {
    return GpuDeviceLossMonitor::instance().withValidatedDeadDomains(
        [&](const GpuValidatedDeadDomains& domains) { return registry.abandonAllNoWait(domains); });
}
