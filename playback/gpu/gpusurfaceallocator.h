#ifndef OLR_GPUSURFACEALLOCATOR_H
#define OLR_GPUSURFACEALLOCATOR_H

#include "playback/gpu/gpubudget.h"
#include "playback/output/framehandle.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

class GpuFence;
class GpuRhiContext;
class GpuSurface;

// Why a GPU mint fell back to a CPU handle. Refusal and exhaustion are different
// faults with different operator responses, so they are never merged into one
// "degraded" bucket: Oom means the allocation could not be paid for, whereas
// SubmissionRefused means a fence/device authority declined to register the
// retirement, so no GPU handle could be published safely.
enum class GpuDegradeCause : uint8_t {
    None = 0,
    Oom,
    SubmissionRefused,
};

struct GpuMintResult {
    FrameHandle handle;
    bool degradedToCpu = false;
    GpuDegradeCause cause = GpuDegradeCause::None;
};

// What a GPU handle factory produced. Implicitly constructible from a FrameHandle
// so a factory that has no submission step stays a plain `return handle;`. A
// factory that does submit must report a refusal with refusedSubmission() rather
// than an empty handle, otherwise the degrade is misfiled as memory pressure.
struct GpuMintAttempt {
    FrameHandle handle;
    bool submissionRefused = false;

    GpuMintAttempt() = default;
    GpuMintAttempt(FrameHandle produced) : handle(std::move(produced)) {}

    static GpuMintAttempt refusedSubmission() {
        GpuMintAttempt attempt;
        attempt.submissionRefused = true;
        return attempt;
    }
};

using GpuFrameHandleFactory =
    std::function<GpuMintAttempt(std::shared_ptr<GpuSurface>, FrameMetadata, GpuBudgetCharge)>;

GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface, FrameMetadata meta,
                               const GpuFrameHandleFactory& gpuFactory,
                               const std::function<CpuPlanes()>& cpuFallback,
                               GpuBudgetTag tag = GpuBudgetTag::DecodeWindow);
GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence,
                               const std::function<CpuPlanes()>& cpuFallback,
                               GpuBudgetTag tag = GpuBudgetTag::DecodeWindow);

#endif // OLR_GPUSURFACEALLOCATOR_H
