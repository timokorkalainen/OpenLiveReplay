#include "playback/gpu/gpusurfaceallocator.h"

#include "playback/gpu/gpubudget.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuopscope.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpuretireregistry.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"

#include <array>
#include <utility>

namespace {

qsizetype planeBytes(int stride, int rows) {
    return static_cast<qsizetype>(stride) * static_cast<qsizetype>(rows);
}

CpuPlanes placeholderPlanesFor(const FrameMetadata& meta) {
    const int width = meta.key.width;
    const int height = meta.key.height;
    if (width <= 0 || height <= 0) return CpuPlanes{};

    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = width;
    planes.height = height;
    planes.stride[0] = width;
    planes.stride[1] = (width + 1) / 2;
    planes.stride[2] = planes.stride[1];
    const int chromaHeight = (height + 1) / 2;
    planes.plane[0] = QByteArray(planeBytes(planes.stride[0], height), char(16));
    planes.plane[1] = QByteArray(planeBytes(planes.stride[1], chromaHeight), char(128));
    planes.plane[2] = QByteArray(planeBytes(planes.stride[2], chromaHeight), char(128));
    return planes;
}

FrameHandle degradeToCpu(FrameMetadata meta, const std::function<CpuPlanes()>& cpuFallback) {
    CpuPlanes planes = cpuFallback ? cpuFallback() : CpuPlanes{};
    if (!planes.isValid()) {
        planes = placeholderPlanesFor(meta);
        meta.key.isPlaceholder = planes.isValid();
    }
    if (!planes.isValid()) return FrameHandle{};
    meta.gpuGeneration = 0;
    return makeCpuFrameHandle(std::move(planes), meta);
}

GpuMintResult degradedResult(FrameMetadata meta, const std::function<CpuPlanes()>& cpuFallback,
                             bool countOomDegrade = true) {
    GpuMintResult result;
    result.handle = degradeToCpu(meta, cpuFallback);
    result.degradedToCpu = result.handle.isPresentable() && !result.handle.isGpuBacked();
    if (countOomDegrade && result.degradedToCpu) GpuBudget::instance().noteOomDegrade();
    return result;
}

} // namespace

GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface, FrameMetadata meta,
                               const GpuFrameHandleFactory& gpuFactory,
                               const std::function<CpuPlanes()>& cpuFallback, GpuBudgetTag tag) {
    GpuMintResult result;

    if (gpuConsumeInjectedAllocFailure()) {
        return degradedResult(meta, cpuFallback);
    }

    if (!surface || !surface->isValid() || !gpuFactory) {
        return degradedResult(meta, cpuFallback);
    }

    const qint64 bytes = gpuSurfaceBytes(*surface);
    if (bytes <= 0) {
        return degradedResult(meta, cpuFallback);
    }
    auto charge = GpuBudget::instance().tryCharge(bytes, tag);
    if (!charge.has_value()) {
        return degradedResult(meta, cpuFallback);
    }
    FrameHandle gpu = gpuFactory(std::move(surface), meta, std::move(*charge));
    if (!gpu.isPresentable()) {
        return degradedResult(meta, cpuFallback);
    }

    result.handle = std::move(gpu);
    result.degradedToCpu = false;
    return result;
}

GpuMintResult mintGpuOrDegrade(std::shared_ptr<GpuSurface> surface,
                               std::shared_ptr<GpuRhiContext> rhi, FrameMetadata meta,
                               std::shared_ptr<GpuFence> renderFence,
                               const std::function<CpuPlanes()>& cpuFallback, GpuBudgetTag tag) {
    if (!rhi || !rhi->isValid()) return degradedResult(meta, cpuFallback);
    return mintGpuOrDegrade(
        std::move(surface), meta,
        [rhi = std::move(rhi), renderFence = std::move(renderFence)](
            std::shared_ptr<GpuSurface> s, FrameMetadata m, GpuBudgetCharge charge) mutable {
            if (s && renderFence) {
                GpuRetireRegistry registry;
                GpuOpScope operation(renderFence, registry);
                auto adapter = []() noexcept { return GpuSubmitOutcome::Submitted; };
                const auto result = operation.submit(
                    adapter, GpuSurfacePack<1>(std::array<std::shared_ptr<GpuSurface>, 1>{s}));
                if (!result.succeeded()) return FrameHandle{};
            }
            return makeGpuFrameHandle(std::move(s), std::move(rhi), m, std::move(renderFence),
                                      std::move(charge));
        },
        cpuFallback, tag);
}
