#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpusurface.h"

#include <array>
#include <memory>

#if defined(OLR_GPU_ADAPTER_D3D11)
struct D3D11SubmissionAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::NotSubmitted; }
};
using TypedSubmissionAdapter = D3D11SubmissionAdapter;
#elif defined(OLR_GPU_ADAPTER_METAL)
struct MetalSubmissionAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::NotSubmitted; }
};
using TypedSubmissionAdapter = MetalSubmissionAdapter;
#else
struct StubSubmissionAdapter {
    GpuSubmitOutcome operator()() noexcept { return GpuSubmitOutcome::NotSubmitted; }
};
using TypedSubmissionAdapter = StubSubmissionAdapter;
#endif

#if defined(OLR_GPU_SUBMISSION_PASS)
GpuSubmissionResult typedSubmissionPass(std::shared_ptr<GpuFence> fence,
                                        std::shared_ptr<GpuSurface> surface) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    TypedSubmissionAdapter adapter;
    return operation.submit(
        adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{std::move(surface)}));
}
#else
GpuSubmissionResult forbiddenOwnerlessSubmission(std::shared_ptr<GpuFence> fence) {
    GpuRetireRegistry registry;
    GpuOpScope operation(std::move(fence), registry);
    TypedSubmissionAdapter adapter;
    return operation.submit(adapter);
}
#endif
