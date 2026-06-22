#include "playback/gpu/gpucompositor.h"

#include "playback/gpu/gpugeneration.h"
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
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxGridSources = 16;

struct GridUniformBlock {
    qint32 matrix = 0;
    qint32 range = 0;
    qint32 columns = 1;
    qint32 rows = 1;
    qint32 sourceSize[kMaxGridSources][4] = {};
};
static_assert(sizeof(GridUniformBlock) == 272);

struct PreparedSource {
    CpuPlanes nv12;
    bool present = false;
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

qsizetype byteOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

QByteArray tightRows(const QByteArray& plane, int stride, int rows, int bytesPerRow) {
    if (rows <= 0 || bytesPerRow <= 0 || stride < bytesPerRow) return {};
    const qsizetype lastByte = byteOffset(rows - 1, stride) + static_cast<qsizetype>(bytesPerRow);
    if (plane.size() < lastByte) return {};

    QByteArray out(static_cast<qsizetype>(rows) * bytesPerRow, 0);
    for (int row = 0; row < rows; ++row) {
        std::memcpy(out.data() + static_cast<qsizetype>(row) * bytesPerRow,
                    plane.constData() + byteOffset(row, stride), bytesPerRow);
    }
    return out;
}

CpuPlanes readFrameAsNv12ForSampling(const FrameHandle& frame) {
    CpuPlanes nv12 = frame.readToCpu(FramePixelFormat::Nv12);
    if (nv12.format == FramePixelFormat::Nv12 && nv12.isValid()) return nv12;
    return formatcanon::yuv420pToNv12(frame.readToCpu(FramePixelFormat::Yuv420p));
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
            source.nv12 = readFrameAsNv12ForSampling(frames.at(i));
            source.present = source.nv12.isValid() && source.nv12.format == FramePixelFormat::Nv12;
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

GridUniformBlock makeUniforms(const QList<PreparedSource>& sources, int frameCount,
                              ColorMetadata color) {
    GridUniformBlock ub;
    ub.matrix = color.matrix == ColorMatrix::Bt601 ? 0 : 1;
    ub.range = color.range == ColorRange::Video ? 1 : 0;
    ub.columns = gridColumnsForCount(frameCount);
    ub.rows = gridRowsForCount(frameCount, ub.columns);
    for (int i = 0; i < qMin(kMaxGridSources, sources.size()); ++i) {
        const PreparedSource& source = sources.at(i);
        if (!source.present) continue;
        ub.sourceSize[i][0] = source.nv12.width;
        ub.sourceSize[i][1] = source.nv12.height;
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

CpuPlanes renderGridWithRhi(QRhi* rhi, const QList<FrameHandle>& frames, int width, int height,
                            ColorMetadata color, GpuCompositor::ScaleQuality quality) {
    if (!rhi || width <= 0 || height <= 0) return {};
    if (!rhi->isTextureFormatSupported(QRhiTexture::RGBA8, QRhiTexture::RenderTarget) ||
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

    const QList<PreparedSource> sources = prepareSources(frames);
    const GridUniformBlock uniforms = makeUniforms(sources, cappedFrameCount(frames), color);

    std::unique_ptr<QRhiTexture> output(
        rhi->newTexture(QRhiTexture::RGBA8, QSize(width, height), 1,
                        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!output || !output->create()) return {};

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
    lumaTextures.reserve(kMaxGridSources);
    chromaTextures.reserve(kMaxGridSources);

    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    if (!updates) return {};
    updates->updateDynamicBuffer(ubuf.get(), 0, sizeof(GridUniformBlock), &uniforms);

    for (int i = 0; i < kMaxGridSources; ++i) {
        const PreparedSource& source = sources.at(i);
        const int srcW = source.present ? source.nv12.width : 1;
        const int srcH = source.present ? source.nv12.height : 1;
        const int chromaW = source.present ? (srcW + 1) / 2 : 1;
        const int chromaH = source.present ? (srcH + 1) / 2 : 1;
        QByteArray yBytes = source.present
                                ? tightRows(source.nv12.plane[0], source.nv12.stride[0], srcH, srcW)
                                : QByteArray(1, char(16));
        QByteArray uvBytes = source.present ? tightRows(source.nv12.plane[1], source.nv12.stride[1],
                                                        chromaH, chromaW * 2)
                                            : QByteArray(2, char(128));
        if (yBytes.isEmpty() || uvBytes.isEmpty()) {
            updates->release();
            return {};
        }

        std::unique_ptr<QRhiTexture> yTex(rhi->newTexture(QRhiTexture::R8, QSize(srcW, srcH)));
        std::unique_ptr<QRhiTexture> uvTex(
            rhi->newTexture(QRhiTexture::RG8, QSize(chromaW, chromaH)));
        if (!yTex || !uvTex || !yTex->create() || !uvTex->create()) {
            updates->release();
            return {};
        }

        QRhiTextureSubresourceUploadDescription yUpload(yBytes);
        yUpload.setDataStride(static_cast<quint32>(srcW));
        yUpload.setSourceSize(QSize(srcW, srcH));
        updates->uploadTexture(yTex.get(), QRhiTextureUploadDescription({{0, 0, yUpload}}));

        QRhiTextureSubresourceUploadDescription uvUpload(uvBytes);
        uvUpload.setDataStride(static_cast<quint32>(chromaW * 2));
        uvUpload.setSourceSize(QSize(chromaW, chromaH));
        updates->uploadTexture(uvTex.get(), QRhiTextureUploadDescription({{0, 0, uvUpload}}));

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
    srb->setBindings(bindings.cbegin(), bindings.cend());
    if (!srb->create()) {
        updates->release();
        return {};
    }

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(rhi->newGraphicsPipeline());
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, frag}});
    QRhiVertexInputLayout inputLayout;
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(srb.get());
    pipeline->setRenderPassDescriptor(rpDesc.get());
    if (!pipeline->create()) {
        updates->release();
        return {};
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
    QRhiResourceUpdateBatch* readbackUpdates = rhi->nextResourceUpdateBatch();
    if (readbackUpdates) {
        readbackUpdates->readBackTexture(QRhiReadbackDescription(output.get()), &readback);
    }
    cb->endPass(readbackUpdates);

    if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess || !readbackUpdates) return {};
    if (readback.format != QRhiTexture::RGBA8 || readback.pixelSize != QSize(width, height)) {
        return {};
    }
    return makeRgbaPlanes(width, height, std::move(readback.data));
}

} // namespace

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
    const uint64_t generation = GpuGenerationCounter::instance().current();
    CpuPlanes rgba = composeGridToCpu(frames, width, height, color, quality);
    if (!rgba.isValid()) return FrameHandle{};
    return makeCpuFrameHandle(std::move(rgba), makeCompositeMetadata(width, height, generation));
}

FrameHandle GpuCompositor::composeGridMemoized(const QList<FrameHandle>& frames, int width,
                                               int height, ColorMetadata color,
                                               ScaleQuality quality,
                                               const QVector<qint64>& sourceKeys,
                                               MultiviewComposite* memo) const {
    if (memo && memo->valid && memo->sourceKeys == sourceKeys && !memo->video.isNull()) {
        // FENCE: a memo hit returns the same handle so its render/readback fence metadata
        // travels with the payload; consumers still observe the original ordering contract.
        return memo->video;
    }

    FrameHandle rendered = composeGrid(frames, width, height, color, quality);
    if (!rendered.isNull() && memo) {
        memo->valid = true;
        memo->sourceKeys = sourceKeys;
        memo->video = rendered;
    }
    return rendered;
}

CpuPlanes GpuCompositor::composeGridToCpu(const QList<FrameHandle>& frames, int width, int height,
                                          ColorMetadata color, ScaleQuality quality) const {
    if (!isValid() || width <= 0 || height <= 0) return CpuPlanes{};

    const uint64_t generation = GpuGenerationCounter::instance().current();
    const QList<FrameHandle> filtered = dropStaleInputs(frames, generation);
    if (!m_impl->rhi->isNullBackend()) {
        CpuPlanes gpu;
        const bool invoked = m_impl->rhi->invokeOnRenderThread([&](QRhi* rhi) {
            // LOCK RULE: all QRhi resources are created, used, and destroyed on the
            // GpuRhiContext render thread. The cadence/output thread only observes
            // the completed readback bytes returned from this synchronous test path.
            gpu = renderGridWithRhi(rhi, filtered, width, height, color, quality);
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
    return composeGrid(QList<FrameHandle>{source}, width, height, color, quality);
}
