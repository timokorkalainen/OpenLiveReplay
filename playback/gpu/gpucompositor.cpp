#include "playback/gpu/gpucompositor.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpucompositor_platform.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpureadbackretainer.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/formatcanon.h"
#include "playback/output/outputbusengine.h"

#include <QColor>
#include <QFile>
#include <QSize>
#include <QVector>
#include <rhi/qshader.h>
#include <rhi/qrhi.h>

#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxGridSources = 16;

struct GridUniformBlock {
    qint32 matrix = 0;
    qint32 range = 0;
    qint32 columns = 1;
    qint32 rows = 1;
    qint32 outputSize[4] = {};
    qint32 sourceSize[kMaxGridSources][4] = {};
    qint32 tileRect[kMaxGridSources][4] = {};
};
static_assert(sizeof(GridUniformBlock) == 544);

struct PreparedSource {
    std::shared_ptr<GpuSurface> surface;
    CpuPlanes nv12;
    GpuSurfaceDesc desc;
    bool present = false;
    bool uploadFromCpu = false;
    bool unsupported = false;
};

struct RenderGridResult {
    CpuPlanes readback;
    bool rendered = false;
};

QList<FrameHandle> dropStaleInputs(const QList<FrameHandle>& frames, uint64_t generation) {
    QList<FrameHandle> filtered;
    filtered.reserve(frames.size());
    for (const FrameHandle& frame : frames) {
        filtered.append(frame.isStaleForGeneration(generation) ? FrameHandle{} : frame);
    }
    return filtered;
}

FrameMetadata makeCompositeMetadata(int width, int height, uint64_t generation) {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Rgba8;
    meta.key.width = width;
    meta.key.height = height;
    meta.stride[0] = width * 4;
    meta.gpuGeneration = generation;
    return meta;
}

QShader loadShader(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

int cappedFrameCount(const QList<FrameHandle>& frames) {
    return static_cast<int>(qMin<qsizetype>(kMaxGridSources, frames.size()));
}

QList<PreparedSource> prepareSources(const QList<FrameHandle>& frames) {
    QList<PreparedSource> sources;
    sources.reserve(kMaxGridSources);
    const int count = cappedFrameCount(frames);
    for (int i = 0; i < count; ++i) {
        PreparedSource source;
        if (!frames.at(i).isNull()) {
            source.surface = gpucompositor::makeInputNv12Surface(frames.at(i));
            source.desc = source.surface ? source.surface->desc() : GpuSurfaceDesc{};
            source.present = source.surface && source.surface->isValid() &&
                             source.desc.format == FramePixelFormat::Nv12 &&
                             source.desc.width > 0 && source.desc.height > 0;
            if (!source.present && !frames.at(i).isGpuBacked()) {
                source.nv12 = frames.at(i).readToCpu(FramePixelFormat::Nv12);
                source.present = source.nv12.isValid() &&
                                 source.nv12.format == FramePixelFormat::Nv12 &&
                                 source.nv12.width > 0 && source.nv12.height > 0;
                source.uploadFromCpu = source.present;
                if (source.present) {
                    source.desc.format = FramePixelFormat::Nv12;
                    source.desc.width = source.nv12.width;
                    source.desc.height = source.nv12.height;
                }
            }
            if (!source.present && frames.at(i).isGpuBacked()) {
                source.unsupported = true;
            }
        }
        sources.append(std::move(source));
    }
    while (sources.size() < kMaxGridSources) {
        sources.append(PreparedSource{});
    }
    return sources;
}

int gridColumnsForCount(int count) {
    return qMax(1, int(std::ceil(std::sqrt(double(qMax(1, count))))));
}

int gridRowsForCount(int count, int columns) {
    return qMax(1, int(std::ceil(double(qMax(1, count)) / double(columns))));
}

GridUniformBlock makeUniforms(const QList<PreparedSource>& sources, int frameCount, int width,
                              int height, ColorMetadata color) {
    GridUniformBlock ub;
    ub.matrix = color.matrix == ColorMatrix::Bt601 ? 0 : 1;
    ub.range = color.range == ColorRange::Video ? 1 : 0;
    ub.columns = gridColumnsForCount(frameCount);
    ub.rows = gridRowsForCount(frameCount, ub.columns);
    ub.outputSize[0] = width;
    ub.outputSize[1] = height;
    for (int i = 0; i < qMin(kMaxGridSources, sources.size()); ++i) {
        if (i < frameCount) {
            const int col = i % ub.columns;
            const int row = i / ub.columns;
            const int dstX = col * width / ub.columns;
            const int dstY = row * height / ub.rows;
            const int dstRight = (col + 1) * width / ub.columns;
            const int dstBottom = (row + 1) * height / ub.rows;
            ub.tileRect[i][0] = dstX;
            ub.tileRect[i][1] = dstY;
            ub.tileRect[i][2] = qMax(0, dstRight - dstX);
            ub.tileRect[i][3] = qMax(0, dstBottom - dstY);
        }
        const PreparedSource& source = sources.at(i);
        if (!source.present) continue;
        ub.sourceSize[i][0] = source.desc.width;
        ub.sourceSize[i][1] = source.desc.height;
        ub.sourceSize[i][2] = 1;
    }
    return ub;
}

CpuPlanes makeRgbaPlanes(int width, int height, QByteArray bytes) {
    CpuPlanes out;
    out.format = FramePixelFormat::Rgba8;
    out.width = width;
    out.height = height;
    out.stride[0] = width * 4;
    if (bytes.size() < static_cast<qsizetype>(out.stride[0]) * height) return {};
    if (bytes.size() != static_cast<qsizetype>(out.stride[0]) * height) {
        bytes.truncate(static_cast<qsizetype>(out.stride[0]) * height);
    }
    out.plane[0] = std::move(bytes);
    return out;
}

bool uploadNv12Planes(QRhiResourceUpdateBatch* updates, QRhiTexture* yTex, QRhiTexture* uvTex,
                      const CpuPlanes& nv12) {
    if (!updates || !yTex || !uvTex || !nv12.isValid() || nv12.format != FramePixelFormat::Nv12)
        return false;
    const int chromaW = (nv12.width + 1) / 2;
    const int chromaH = (nv12.height + 1) / 2;
    if (nv12.stride[0] < nv12.width || nv12.stride[1] < chromaW * 2) return false;

    QRhiTextureSubresourceUploadDescription yUpload(nv12.plane[0]);
    yUpload.setDataStride(static_cast<quint32>(nv12.stride[0]));
    yUpload.setSourceSize(QSize(nv12.width, nv12.height));
    updates->uploadTexture(yTex, QRhiTextureUploadDescription({{0, 0, yUpload}}));

    QRhiTextureSubresourceUploadDescription uvUpload(nv12.plane[1]);
    uvUpload.setDataStride(static_cast<quint32>(nv12.stride[1]));
    uvUpload.setSourceSize(QSize(chromaW, chromaH));
    updates->uploadTexture(uvTex, QRhiTextureUploadDescription({{0, 0, uvUpload}}));
    return true;
}

RenderGridResult renderGridWithRhi(QRhi* rhi, const QList<PreparedSource>& sources, int frameCount,
                                   int width, int height, ColorMetadata color,
                                   GpuCompositor::ScaleQuality quality,
                                   const std::shared_ptr<GpuSurface>& outputSurface) {
    RenderGridResult result;
    if (!rhi || width <= 0 || height <= 0) return {};
    for (int i = 0; i < qMin(frameCount, sources.size()); ++i) {
        if (sources.at(i).unsupported) return {};
    }
    const QRhiTexture::Format outputFormat =
        outputSurface ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8;
    if (!rhi->isTextureFormatSupported(outputFormat, QRhiTexture::RenderTarget) ||
        !rhi->isTextureFormatSupported(QRhiTexture::R8) ||
        !rhi->isTextureFormatSupported(QRhiTexture::RG8)) {
        return {};
    }

    const QShader vert = loadShader(QStringLiteral(":/olr/shaders/grid.vert.qsb"));
    const QString fragPath = quality == GpuCompositor::ScaleQuality::NearestCompat
                                 ? QStringLiteral(":/olr/shaders/grid_nn.frag.qsb")
                                 : QStringLiteral(":/olr/shaders/grid_quality.frag.qsb");
    const QShader frag = loadShader(fragPath);
    if (!vert.isValid() || !frag.isValid()) return {};

    const GridUniformBlock uniforms = makeUniforms(sources, frameCount, width, height, color);

    std::unique_ptr<gpucompositor::ImportedRgbaRenderTarget> importedOutput;
    std::unique_ptr<QRhiTexture> output;
    if (outputSurface) {
        importedOutput = gpucompositor::importRgbaRenderTarget(rhi, outputSurface);
        if (!importedOutput) return {};
        output.reset(
            rhi->newTexture(outputFormat, QSize(width, height), 1, QRhiTexture::RenderTarget));
        if (!output || !output->createFrom(importedOutput->nativeTexture())) return {};
    } else {
        output.reset(
            rhi->newTexture(outputFormat, QSize(width, height), 1,
                            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!output || !output->create()) return {};
    }

    std::unique_ptr<QRhiTextureRenderTarget> renderTarget(rhi->newTextureRenderTarget(
        QRhiTextureRenderTargetDescription(QRhiColorAttachment(output.get()))));
    if (!renderTarget) return {};
    std::unique_ptr<QRhiRenderPassDescriptor> rpDesc(
        renderTarget->newCompatibleRenderPassDescriptor());
    renderTarget->setRenderPassDescriptor(rpDesc.get());
    if (!rpDesc || !renderTarget->create()) return {};

    const QRhiSampler::Filter filter = quality == GpuCompositor::ScaleQuality::NearestCompat
                                           ? QRhiSampler::Nearest
                                           : QRhiSampler::Linear;
    std::unique_ptr<QRhiSampler> sampler(rhi->newSampler(
        filter, filter, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!sampler || !sampler->create()) return {};

    std::unique_ptr<QRhiBuffer> ubuf(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(GridUniformBlock)));
    if (!ubuf || !ubuf->create()) return {};

    std::vector<std::unique_ptr<QRhiTexture>> lumaTextures;
    std::vector<std::unique_ptr<QRhiTexture>> chromaTextures;
    std::vector<std::unique_ptr<gpucompositor::ImportedNv12Source>> importedSources;
    lumaTextures.reserve(kMaxGridSources);
    chromaTextures.reserve(kMaxGridSources);
    importedSources.reserve(kMaxGridSources);

    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    if (!updates) return {};
    updates->updateDynamicBuffer(ubuf.get(), 0, sizeof(GridUniformBlock), &uniforms);

    for (int i = 0; i < kMaxGridSources; ++i) {
        const PreparedSource& source = sources.at(i);
        const int srcW = source.present ? source.desc.width : 1;
        const int srcH = source.present ? source.desc.height : 1;
        const int chromaW = source.present ? (srcW + 1) / 2 : 1;
        const int chromaH = source.present ? (srcH + 1) / 2 : 1;
        std::unique_ptr<QRhiTexture> yTex(rhi->newTexture(QRhiTexture::R8, QSize(srcW, srcH)));
        std::unique_ptr<QRhiTexture> uvTex(
            rhi->newTexture(QRhiTexture::RG8, QSize(chromaW, chromaH)));
        if (!yTex || !uvTex) {
            updates->release();
            return {};
        }

        if (source.present && source.surface) {
            auto imported = gpucompositor::importNv12Source(rhi, source.surface);
            if (!imported || !yTex->createFrom(imported->lumaNativeTexture()) ||
                !uvTex->createFrom(imported->chromaNativeTexture())) {
                updates->release();
                return {};
            }
            importedSources.push_back(std::move(imported));
        } else if (source.present && source.uploadFromCpu) {
            if (!yTex->create() || !uvTex->create() ||
                !uploadNv12Planes(updates, yTex.get(), uvTex.get(), source.nv12)) {
                updates->release();
                return {};
            }
        } else {
            if (!yTex->create() || !uvTex->create()) {
                updates->release();
                return {};
            }
            QByteArray yBytes(1, char(16));
            QByteArray uvBytes(2, char(128));

            QRhiTextureSubresourceUploadDescription yUpload(yBytes);
            yUpload.setDataStride(static_cast<quint32>(1));
            yUpload.setSourceSize(QSize(1, 1));
            updates->uploadTexture(yTex.get(), QRhiTextureUploadDescription({{0, 0, yUpload}}));

            QRhiTextureSubresourceUploadDescription uvUpload(uvBytes);
            uvUpload.setDataStride(static_cast<quint32>(2));
            uvUpload.setSourceSize(QSize(1, 1));
            updates->uploadTexture(uvTex.get(), QRhiTextureUploadDescription({{0, 0, uvUpload}}));
        }

        lumaTextures.push_back(std::move(yTex));
        chromaTextures.push_back(std::move(uvTex));
    }

    QVector<QRhiShaderResourceBinding> bindings;
    bindings.reserve(2 + kMaxGridSources * 2);
    bindings.append(QRhiShaderResourceBinding::uniformBuffer(
        0, QRhiShaderResourceBinding::FragmentStage, ubuf.get()));
    for (int i = 0; i < kMaxGridSources; ++i) {
        bindings.append(QRhiShaderResourceBinding::texture(
            1 + i * 2, QRhiShaderResourceBinding::FragmentStage, lumaTextures.at(size_t(i)).get()));
        bindings.append(QRhiShaderResourceBinding::texture(2 + i * 2,
                                                           QRhiShaderResourceBinding::FragmentStage,
                                                           chromaTextures.at(size_t(i)).get()));
    }
    bindings.append(QRhiShaderResourceBinding::sampler(33, QRhiShaderResourceBinding::FragmentStage,
                                                       sampler.get()));

    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
    if (!srb) {
        updates->release();
        return {};
    }
    srb->setBindings(bindings.cbegin(), bindings.cend());
    if (!srb->create()) {
        updates->release();
        return {};
    }

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi->newGraphicsPipeline());
    if (!pipeline) {
        updates->release();
        return {};
    }
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, frag}});
    QRhiVertexInputLayout inputLayout;
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(srb.get());
    pipeline->setRenderPassDescriptor(rpDesc.get());
    if (!pipeline->create()) {
        updates->release();
        return result;
    }

    QRhiReadbackResult readback;
    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        updates->release();
        return {};
    }

    cb->beginPass(renderTarget.get(), QColor(0, 0, 0, 255), {1.0f, 0}, updates);
    cb->setGraphicsPipeline(pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, static_cast<float>(width), static_cast<float>(height)));
    cb->setShaderResources(srb.get());
    cb->draw(3);
    QRhiResourceUpdateBatch* afterPassUpdates = nullptr;
    if (!outputSurface) {
        afterPassUpdates = rhi->nextResourceUpdateBatch();
        if (afterPassUpdates) {
            afterPassUpdates->readBackTexture(QRhiReadbackDescription(output.get()), &readback);
        }
    }
    cb->endPass(afterPassUpdates);

    if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess) return {};
    result.rendered = true;
    if (!outputSurface) {
        if (!afterPassUpdates || readback.format != QRhiTexture::RGBA8 ||
            readback.pixelSize != QSize(width, height)) {
            return {};
        }
        result.readback = makeRgbaPlanes(width, height, std::move(readback.data));
    }
    return result;
}

} // namespace

#ifndef __APPLE__
namespace gpucompositor {

std::shared_ptr<GpuSurface> makeInputNv12Surface(const FrameHandle&) {
    return nullptr;
}

std::shared_ptr<GpuSurface> makeOutputRgba8Surface(int, int) {
    return nullptr;
}

std::unique_ptr<ImportedNv12Source> importNv12Source(QRhi*, const std::shared_ptr<GpuSurface>&) {
    return nullptr;
}

std::unique_ptr<ImportedRgbaRenderTarget>
importRgbaRenderTarget(QRhi*, const std::shared_ptr<GpuSurface>&) {
    return nullptr;
}

} // namespace gpucompositor
#endif

class GpuCompositor::Impl {
public:
    std::shared_ptr<GpuRhiContext> rhi;
};

GpuCompositor::GpuCompositor(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GpuCompositor::~GpuCompositor() = default;

std::shared_ptr<GpuCompositor> GpuCompositor::create(std::shared_ptr<GpuRhiContext> rhi) {
    if (!rhi || !rhi->isValid()) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->rhi = std::move(rhi);
    return std::shared_ptr<GpuCompositor>(new GpuCompositor(std::move(impl)));
}

#ifndef __APPLE__
std::shared_ptr<GpuSurface>
GpuCompositor::uploadFrameToNv12SurfaceForTest(const FrameHandle&,
                                               const std::shared_ptr<GpuRhiContext>&) {
    return nullptr;
}
#endif

bool GpuCompositor::isValid() const {
    return m_impl && m_impl->rhi && m_impl->rhi->isValid();
}

FrameHandle GpuCompositor::composeGrid(const QList<FrameHandle>& frames, int width, int height,
                                       ColorMetadata color, ScaleQuality quality) const {
    return composeGridForGeneration(frames, width, height, color, quality,
                                    GpuGenerationCounter::instance().current());
}

FrameHandle GpuCompositor::composeGridForGeneration(const QList<FrameHandle>& frames, int width,
                                                    int height, ColorMetadata color,
                                                    ScaleQuality quality,
                                                    uint64_t generation) const {
    if (!isValid() || width <= 0 || height <= 0) return FrameHandle{};
    if (frames.size() > kMaxGridSources) return FrameHandle{};
    if (gpuConsumeInjectedAllocFailure()) return FrameHandle{};

    const QList<FrameHandle> filtered = dropStaleInputs(frames, generation);
    if (m_impl && m_impl->rhi && m_impl->rhi->isGpuBacked()) {
        const QList<PreparedSource> sources = prepareSources(filtered);
        std::shared_ptr<GpuSurface> surface = gpucompositor::makeOutputRgba8Surface(width, height);
        if (!surface || !surface->isValid()) return FrameHandle{};

        bool rendered = false;
        const bool invoked = m_impl->rhi->invokeOnRenderThread([&](QRhi* rhi) {
            rendered = renderGridWithRhi(rhi, sources, cappedFrameCount(filtered), width, height,
                                         color, quality, surface)
                           .rendered;
        });
        if (!invoked || !rendered) return FrameHandle{};

        std::shared_ptr<GpuFence> renderFence = m_impl->rhi->createFence();
        if (renderFence) {
            const uint64_t fenceValue = renderFence->signal();
            surface->retainUntilFenceRetired(fenceValue);
            gpuRetainSurfaceUntilFenceRetired(surface, renderFence, fenceValue);
        }
        FrameMetadata meta = makeCompositeMetadata(width, height, generation);
        meta.color = color;
        return makeGpuFrameHandle(std::move(surface), m_impl->rhi, std::move(meta),
                                  std::move(renderFence));
    }

    CpuPlanes rgba =
        composeGridToCpuForGeneration(filtered, width, height, color, quality, generation);
    if (!rgba.isValid()) return FrameHandle{};
    return makeCpuFrameHandle(std::move(rgba), makeCompositeMetadata(width, height, generation));
}

FrameHandle GpuCompositor::composeGridMemoized(const QList<FrameHandle>& frames, int width,
                                               int height, ColorMetadata color,
                                               ScaleQuality quality,
                                               const QVector<qint64>& sourceKeys,
                                               MultiviewComposite* memo) const {
    return composeGridMemoizedForGeneration(frames, width, height, color, quality, sourceKeys, memo,
                                            GpuGenerationCounter::instance().current());
}

FrameHandle GpuCompositor::composeGridMemoizedForGeneration(
    const QList<FrameHandle>& frames, int width, int height, ColorMetadata color,
    ScaleQuality quality, const QVector<qint64>& sourceKeys, MultiviewComposite* memo,
    uint64_t generation) const {
    if (memo && memo->valid && memo->sourceKeys == sourceKeys && !memo->video.isNull()) {
        // FENCE: a memo hit returns the same handle so its render/readback fence metadata
        // travels with the payload; consumers still observe the original ordering contract.
        return memo->video;
    }

    FrameHandle rendered =
        composeGridForGeneration(frames, width, height, color, quality, generation);
    if (!rendered.isNull() && memo) {
        memo->valid = true;
        memo->sourceKeys = sourceKeys;
        memo->video = rendered;
    }
    return rendered;
}

CpuPlanes GpuCompositor::composeGridToCpu(const QList<FrameHandle>& frames, int width, int height,
                                          ColorMetadata color, ScaleQuality quality) const {
    return composeGridToCpuForGeneration(frames, width, height, color, quality,
                                         GpuGenerationCounter::instance().current());
}

CpuPlanes GpuCompositor::composeGridToCpuForGeneration(const QList<FrameHandle>& frames, int width,
                                                       int height, ColorMetadata color,
                                                       ScaleQuality quality,
                                                       uint64_t generation) const {
    if (!isValid() || width <= 0 || height <= 0) return CpuPlanes{};
    if (frames.size() > kMaxGridSources) return CpuPlanes{};

    const QList<FrameHandle> filtered = dropStaleInputs(frames, generation);
    if (!m_impl->rhi->isNullBackend()) {
        const QList<PreparedSource> sources = prepareSources(filtered);
        CpuPlanes gpu;
        const bool invoked = m_impl->rhi->invokeOnRenderThread([&](QRhi* rhi) {
            // LOCK RULE: all QRhi resources are created, used, and destroyed on the
            // GpuRhiContext render thread. The cadence/output thread only observes
            // the completed readback bytes returned from this synchronous test path.
            gpu = renderGridWithRhi(rhi, sources, cappedFrameCount(filtered), width, height, color,
                                    quality, nullptr)
                      .readback;
        });
        return invoked ? gpu : CpuPlanes{};
    }

    if (quality != ScaleQuality::NearestCompat) return CpuPlanes{};
    CpuPlanes rgba = formatcanon::referenceComposeGridRgba8(filtered, width, height, color);
    if (rgba.isValid()) {
        rgba.format = FramePixelFormat::Rgba8;
    }
    return rgba;
}

FrameHandle GpuCompositor::composePgm(const FrameHandle& source, int width, int height,
                                      ColorMetadata color, ScaleQuality quality) const {
    return composePgmForGeneration(source, width, height, color, quality,
                                   GpuGenerationCounter::instance().current());
}

FrameHandle GpuCompositor::composePgmForGeneration(const FrameHandle& source, int width, int height,
                                                   ColorMetadata color, ScaleQuality quality,
                                                   uint64_t generation) const {
    return composeGridForGeneration(QList<FrameHandle>{source}, width, height, color, quality,
                                    generation);
}
