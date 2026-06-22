// GpuCompositor is the RHI grid/PGM compositor; the CPU Yuv420pCompositor stays
// the oracle + fallback. With no RHI backend create() may return nullptr; with a
// backend it must never return a partially initialized compositor.
#include <QtTest>

#include <QFile>
#include <rhi/qshader.h>

#include "framepsnr.h"

#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/formatcanon.h"
#include "playback/output/framehandle.h"
#include "playback/output/outputbusengine.h"

#include <utility>

class TestGpuCompositor : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void gridShadersLoadAndBuildPipeline();
    void compatGridFallsBackToCpuOracleOnNull();
    void compatGridMatchesCpuOracleExactOnWarp();
    void compatGridWithinOneLsbOnLocalGpu();
    void composeGridReturnsGpuBackedRgbaOnLocalGpu();
    void swappedQuadrantsDifferFromOracle();
    void pgmSelectFillsOutputFromSingleSource();
    void bilinearScalerMeetsPsnrAgainstBilinearReference();
    void bilinearGridMeetsPsnrAgainstBilinearReferenceOnWarp();
    void bilinearDiffersFromNearestOracle();
    void memoHitReusesSameGpuSurface();
    void memoMissRendersFresh();
#ifdef __APPLE__
    void cpuHandleUploadsToNv12Surface();
    void gpuNv12HandleAliasesExistingSurface();
#endif
};

namespace {

qsizetype planeOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

FrameHandle patternedYuv420pHandle(int width, int height) {
    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;

    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = width;
    planes.height = height;
    planes.stride[0] = width + 5;
    planes.stride[1] = chromaW + 3;
    planes.stride[2] = chromaW + 7;
    planes.plane[0] = QByteArray(planeOffset(height, planes.stride[0]), char(0x7f));
    planes.plane[1] = QByteArray(planeOffset(chromaH, planes.stride[1]), char(0x55));
    planes.plane[2] = QByteArray(planeOffset(chromaH, planes.stride[2]), char(0xaa));

    for (int row = 0; row < height; ++row) {
        char* y = planes.plane[0].data() + planeOffset(row, planes.stride[0]);
        for (int x = 0; x < width; ++x) {
            y[x] = char((17 + row * 19 + x * 11) & 0xff);
        }
    }
    for (int row = 0; row < chromaH; ++row) {
        char* u = planes.plane[1].data() + planeOffset(row, planes.stride[1]);
        char* v = planes.plane[2].data() + planeOffset(row, planes.stride[2]);
        for (int x = 0; x < chromaW; ++x) {
            u[x] = char((61 + row * 23 + x * 13) & 0xff);
            v[x] = char((193 - row * 17 - x * 7) & 0xff);
        }
    }

    FrameMetadata meta;
    meta.key.feedIndex = 2;
    meta.key.ptsMs = 80;
    return makeCpuFrameHandle(std::move(planes), meta);
}

FrameHandle gradientYuv420pHandle(int width, int height) {
    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;

    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = width;
    planes.height = height;
    planes.stride[0] = width;
    planes.stride[1] = chromaW;
    planes.stride[2] = chromaW;
    planes.plane[0] = QByteArray(planeOffset(height, planes.stride[0]), 0);
    planes.plane[1] = QByteArray(planeOffset(chromaH, planes.stride[1]), 0);
    planes.plane[2] = QByteArray(planeOffset(chromaH, planes.stride[2]), 0);

    for (int y = 0; y < height; ++y) {
        char* row = planes.plane[0].data() + planeOffset(y, planes.stride[0]);
        for (int x = 0; x < width; ++x) {
            row[x] = char(32 + ((x * 149 + y * 53) / qMax(1, width + height - 2)));
        }
    }
    for (int y = 0; y < chromaH; ++y) {
        char* u = planes.plane[1].data() + planeOffset(y, planes.stride[1]);
        char* v = planes.plane[2].data() + planeOffset(y, planes.stride[2]);
        for (int x = 0; x < chromaW; ++x) {
            u[x] = char(92 + ((x * 31 + y * 17) / qMax(1, chromaW + chromaH - 2)));
            v[x] = char(156 - ((x * 23 + y * 11) / qMax(1, chromaW + chromaH - 2)));
        }
    }

    FrameMetadata meta;
    return makeCpuFrameHandle(std::move(planes), meta);
}

QList<FrameHandle> nonDivisibleGridFrames() {
    return {
        patternedYuv420pHandle(6, 5), gradientYuv420pHandle(5, 7),  patternedYuv420pHandle(7, 4),
        gradientYuv420pHandle(3, 6),  patternedYuv420pHandle(4, 9),
    };
}

bool warpRequiredForTest() {
    return qEnvironmentVariableIntValue("OLR_REQUIRE_WARP") != 0;
}

void compareRows(const CpuPlanes& expected, const CpuPlanes& actual, int plane, int rows,
                 int bytesPerRow) {
    for (int row = 0; row < rows; ++row) {
        const QByteArrayView exp(expected.plane[plane].constData() +
                                     planeOffset(row, expected.stride[plane]),
                                 bytesPerRow);
        const QByteArrayView got(
            actual.plane[plane].constData() + planeOffset(row, actual.stride[plane]), bytesPerRow);
        QCOMPARE(got, exp);
    }
}

double sampleLinear(const QByteArray& plane, int stride, int width, int height, int bytesPerPixel,
                    int channel, double u, double v) {
    const double fx = u * double(width) - 0.5;
    const double fy = v * double(height) - 0.5;
    const int x0 = qBound(0, int(std::floor(fx)), width - 1);
    const int y0 = qBound(0, int(std::floor(fy)), height - 1);
    const int x1 = qBound(0, x0 + 1, width - 1);
    const int y1 = qBound(0, y0 + 1, height - 1);
    const double tx = qBound(0.0, fx - std::floor(fx), 1.0);
    const double ty = qBound(0.0, fy - std::floor(fy), 1.0);

    auto at = [&](int x, int y) {
        const qsizetype offset =
            planeOffset(y, stride) + static_cast<qsizetype>(x) * bytesPerPixel + channel;
        return double(uchar(plane.at(offset)));
    };

    const double a = at(x0, y0) * (1.0 - tx) + at(x1, y0) * tx;
    const double b = at(x0, y1) * (1.0 - tx) + at(x1, y1) * tx;
    return a * (1.0 - ty) + b * ty;
}

CpuPlanes bilinearUpscaleReferenceRgba8(const FrameHandle& source, int width, int height,
                                        ColorMetadata color) {
    const CpuPlanes nv12 = source.readToCpu(FramePixelFormat::Nv12);
    if (!nv12.isValid() || nv12.format != FramePixelFormat::Nv12) return {};

    const int chromaW = (nv12.width + 1) / 2;
    const int chromaH = (nv12.height + 1) / 2;
    CpuPlanes out;
    out.format = FramePixelFormat::Rgba8;
    out.width = width;
    out.height = height;
    out.stride[0] = width * 4;
    out.plane[0] = QByteArray(planeOffset(height, out.stride[0]), 0);

    for (int y = 0; y < height; ++y) {
        char* dst = out.plane[0].data() + planeOffset(y, out.stride[0]);
        const double v = (double(y) + 0.5) / double(height);
        for (int x = 0; x < width; ++x) {
            const double u = (double(x) + 0.5) / double(width);
            const int yy = qRound(
                sampleLinear(nv12.plane[0], nv12.stride[0], nv12.width, nv12.height, 1, 0, u, v));
            const int cb =
                qRound(sampleLinear(nv12.plane[1], nv12.stride[1], chromaW, chromaH, 2, 0, u, v));
            const int cr =
                qRound(sampleLinear(nv12.plane[1], nv12.stride[1], chromaW, chromaH, 2, 1, u, v));
            const formatcanon::Rgb8 rgb =
                formatcanon::yuvToRgb8(uchar(qBound(0, yy, 255)), uchar(qBound(0, cb, 255)),
                                       uchar(qBound(0, cr, 255)), color.matrix, color.range);
            const qsizetype offset = static_cast<qsizetype>(x) * 4;
            dst[offset] = char(rgb.r);
            dst[offset + 1] = char(rgb.g);
            dst[offset + 2] = char(rgb.b);
            dst[offset + 3] = char(255);
        }
    }
    return out;
}

CpuPlanes bilinearGridReferenceRgba8(const QList<FrameHandle>& frames, int width, int height,
                                     ColorMetadata color) {
    CpuPlanes out;
    out.format = FramePixelFormat::Rgba8;
    out.width = width;
    out.height = height;
    out.stride[0] = width * 4;
    out.plane[0] = QByteArray(planeOffset(height, out.stride[0]), 0);

    const formatcanon::Rgb8 bg = formatcanon::yuvToRgb8(16, 128, 128, color.matrix, color.range);
    for (int y = 0; y < height; ++y) {
        char* dst = out.plane[0].data() + planeOffset(y, out.stride[0]);
        for (int x = 0; x < width; ++x) {
            const qsizetype offset = static_cast<qsizetype>(x) * 4;
            dst[offset] = char(bg.r);
            dst[offset + 1] = char(bg.g);
            dst[offset + 2] = char(bg.b);
            dst[offset + 3] = char(255);
        }
    }

    const int count = qMax(1, static_cast<int>(frames.size()));
    const int columns = qMax(1, int(std::ceil(std::sqrt(double(count)))));
    const int rows = qMax(1, int(std::ceil(double(count) / double(columns))));

    for (int i = 0; i < static_cast<int>(frames.size()); ++i) {
        const CpuPlanes nv12 = frames.at(i).readToCpu(FramePixelFormat::Nv12);
        if (!nv12.isValid() || nv12.format != FramePixelFormat::Nv12) continue;

        const int srcW = nv12.width;
        const int srcH = nv12.height;
        const int chromaW = (srcW + 1) / 2;
        const int chromaH = (srcH + 1) / 2;
        if (srcW <= 0 || srcH <= 0 || chromaW <= 0 || chromaH <= 0) continue;

        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * width / columns;
        const int dstY = row * height / rows;
        const int dstRight = (col + 1) * width / columns;
        const int dstBottom = (row + 1) * height / rows;
        const int dstW = qMax(0, dstRight - dstX);
        const int dstH = qMax(0, dstBottom - dstY);
        if (dstW <= 0 || dstH <= 0) continue;

        for (int y = 0; y < dstH; ++y) {
            char* dst = out.plane[0].data() + planeOffset(dstY + y, out.stride[0]);
            const double v = (double(y) + 0.5) / double(dstH);
            for (int x = 0; x < dstW; ++x) {
                const double u = (double(x) + 0.5) / double(dstW);
                const int yy =
                    qRound(sampleLinear(nv12.plane[0], nv12.stride[0], srcW, srcH, 1, 0, u, v));
                const int cb = qRound(
                    sampleLinear(nv12.plane[1], nv12.stride[1], chromaW, chromaH, 2, 0, u, v));
                const int cr = qRound(
                    sampleLinear(nv12.plane[1], nv12.stride[1], chromaW, chromaH, 2, 1, u, v));
                const formatcanon::Rgb8 rgb =
                    formatcanon::yuvToRgb8(uchar(qBound(0, yy, 255)), uchar(qBound(0, cb, 255)),
                                           uchar(qBound(0, cr, 255)), color.matrix, color.range);
                const qsizetype offset = static_cast<qsizetype>(dstX + x) * 4;
                dst[offset] = char(rgb.r);
                dst[offset + 1] = char(rgb.g);
                dst[offset + 2] = char(rgb.b);
                dst[offset + 3] = char(255);
            }
        }
    }
    return out;
}

QByteArray packChannel(const CpuPlanes& rgba, int channel) {
    QByteArray out(planeOffset(rgba.height, rgba.width), 0);
    for (int y = 0; y < rgba.height; ++y) {
        const char* src = rgba.plane[0].constData() + planeOffset(y, rgba.stride[0]);
        char* dst = out.data() + planeOffset(y, rgba.width);
        for (int x = 0; x < rgba.width; ++x) {
            dst[x] = src[static_cast<qsizetype>(x) * 4 + channel];
        }
    }
    return out;
}

void compareRgbaWithinOneLsb(const CpuPlanes& actual, const CpuPlanes& expected) {
    QCOMPARE(actual.format, FramePixelFormat::Rgba8);
    QCOMPARE(actual.width, expected.width);
    QCOMPARE(actual.height, expected.height);
    QCOMPARE(actual.stride[0], expected.stride[0]);
    QCOMPARE(actual.plane[0].size(), expected.plane[0].size());

    for (qsizetype i = 0; i < expected.plane[0].size(); ++i) {
        const int got = uchar(actual.plane[0].at(i));
        const int exp = uchar(expected.plane[0].at(i));
        QVERIFY2(
            qAbs(got - exp) <= 1,
            qPrintable(QStringLiteral("RGBA byte %1 got %2 expected %3").arg(i).arg(got).arg(exp)));
    }
}

} // namespace

void TestGpuCompositor::createIsNullOrValidNeverPartial() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor pipeline unavailable on this host");

    QVERIFY(comp->isValid());
}

void TestGpuCompositor::gridShadersLoadAndBuildPipeline() {
    auto load = [](const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
    };

    const QShader vert = load(QStringLiteral(":/olr/shaders/grid.vert.qsb"));
    const QShader frag = load(QStringLiteral(":/olr/shaders/grid_nn.frag.qsb"));
    const QShader quality = load(QStringLiteral(":/olr/shaders/grid_quality.frag.qsb"));

    QVERIFY2(vert.isValid(), "grid.vert.qsb missing or not deserializable");
    QVERIFY2(frag.isValid(), "grid_nn.frag.qsb missing or not deserializable");
    QVERIFY2(quality.isValid(), "grid_quality.frag.qsb missing or not deserializable");
    QCOMPARE(vert.stage(), QShader::VertexStage);
    QCOMPARE(frag.stage(), QShader::FragmentStage);
    QCOMPARE(quality.stage(), QShader::FragmentStage);
}

void TestGpuCompositor::compatGridFallsBackToCpuOracleOnNull() {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) QSKIP("QRhi Null backend unavailable on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> frames{
        solidYuv420pHandle(4, 4, 40, 60, 200),
        solidYuv420pHandle(4, 4, 80, 70, 190),
        solidYuv420pHandle(4, 4, 120, 80, 180),
        solidYuv420pHandle(4, 4, 160, 90, 170),
    };
    ColorMetadata color;

    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    QVERIFY(oracle.isValid());
    const CpuPlanes gpu =
        comp->composeGridToCpu(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(gpu.isValid());
    QCOMPARE(gpu.format, FramePixelFormat::Rgba8);
    QCOMPARE(gpu.width, 8);
    QCOMPARE(gpu.height, 8);
    QCOMPARE(gpu.plane[0], oracle.plane[0]);
}

void TestGpuCompositor::compatGridMatchesCpuOracleExactOnWarp() {
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi) {
        if (warpRequiredForTest()) QFAIL("required QRhi WARP/D3D11 backend unavailable");
        QSKIP("QRhi WARP/D3D11 backend unavailable on this host");
    }
    QVERIFY(rhi->isGpuBacked());
    QVERIFY(!rhi->isNullBackend());

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    const QList<FrameHandle> frames = nonDivisibleGridFrames();
    ColorMetadata color;

    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 17, 11, color);
    QVERIFY(oracle.isValid());
    const CpuPlanes gpu =
        comp->composeGridToCpu(frames, 17, 11, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(gpu.isValid());
    QCOMPARE(gpu.format, FramePixelFormat::Rgba8);
    QCOMPARE(gpu.width, 17);
    QCOMPARE(gpu.height, 11);
    QCOMPARE(gpu.plane[0], oracle.plane[0]);
}

void TestGpuCompositor::compatGridWithinOneLsbOnLocalGpu() {
    auto rhi = GpuRhiContext::create();
    if (!rhi || !rhi->isGpuBacked()) QSKIP("no local GPU backend on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    const QList<FrameHandle> frames = nonDivisibleGridFrames();
    ColorMetadata color;
    color.range = ColorRange::Full;

    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 17, 11, color);
    QVERIFY(oracle.isValid());
    const CpuPlanes gpu =
        comp->composeGridToCpu(frames, 17, 11, color, GpuCompositor::ScaleQuality::NearestCompat);
    if (!gpu.isValid()) QSKIP("local GPU grid render/readback unavailable on this host");
    compareRgbaWithinOneLsb(gpu, oracle);
}

void TestGpuCompositor::composeGridReturnsGpuBackedRgbaOnLocalGpu() {
#ifndef __APPLE__
    QSKIP("GPU-backed compositor output surfaces are Apple-only in this phase");
#else
    auto rhi = GpuRhiContext::create();
    if (!rhi || !rhi->isGpuBacked()) QSKIP("no local GPU backend on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> frames{
        solidYuv420pHandle(4, 4, 40, 60, 200),
        solidYuv420pHandle(4, 4, 160, 90, 170),
    };
    ColorMetadata color;
    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    QVERIFY(oracle.isValid());

    const FrameHandle gpu =
        comp->composeGrid(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(!gpu.isNull());
    QVERIFY(gpu.isGpuBacked());
    QCOMPARE(gpu.metadata().key.format, FramePixelFormat::Rgba8);
    QCOMPARE(gpu.data()->nativeFormat(), FramePixelFormat::Rgba8);
    QVERIFY(gpu.data()->gpuSurface() != nullptr);
    QCOMPARE(gpu.data()->gpuSurface()->desc().format, FramePixelFormat::Rgba8);

    const CpuPlanes rgba = gpu.readToCpu(FramePixelFormat::Rgba8);
    QVERIFY(rgba.isValid());
    compareRgbaWithinOneLsb(rgba, oracle);

    const CpuPlanes yuv = gpu.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(yuv.isValid());
    QCOMPARE(yuv.format, FramePixelFormat::Yuv420p);
    QCOMPARE(yuv.width, 8);
    QCOMPARE(yuv.height, 8);
#endif
}

void TestGpuCompositor::swappedQuadrantsDifferFromOracle() {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) QSKIP("QRhi Null backend unavailable on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> correct{
        solidYuv420pHandle(4, 4, 40, 60, 200),
        solidYuv420pHandle(4, 4, 80, 70, 190),
        solidYuv420pHandle(4, 4, 120, 80, 180),
        solidYuv420pHandle(4, 4, 160, 90, 170),
    };
    QList<FrameHandle> swapped{correct.at(1), correct.at(0), correct.at(2), correct.at(3)};
    ColorMetadata color;

    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(correct, 8, 8, color);
    const CpuPlanes gpu =
        comp->composeGridToCpu(swapped, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(oracle.isValid());
    QVERIFY(gpu.isValid());
    QVERIFY(gpu.plane[0] != oracle.plane[0]);
}

void TestGpuCompositor::pgmSelectFillsOutputFromSingleSource() {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) QSKIP("QRhi Null backend unavailable on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    const FrameHandle source = solidYuv420pHandle(4, 4, 92, 108, 154);
    ColorMetadata color;
    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8({source}, 8, 8, color);
    QVERIFY(oracle.isValid());

    const FrameHandle pgm =
        comp->composePgm(source, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(!pgm.isNull());
    const CpuPlanes got = pgm.readToCpu(FramePixelFormat::Rgba8);
    QVERIFY(got.isValid());
    QCOMPARE(got.plane[0], oracle.plane[0]);
}

void TestGpuCompositor::bilinearScalerMeetsPsnrAgainstBilinearReference() {
    auto rhi = GpuRhiContext::create();
    if (!rhi || !rhi->isGpuBacked()) QSKIP("no local GPU backend on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    const FrameHandle src = gradientYuv420pHandle(8, 8);
    ColorMetadata color;
    const CpuPlanes ref = bilinearUpscaleReferenceRgba8(src, 32, 32, color);
    QVERIFY(ref.isValid());
    const CpuPlanes gpu =
        comp->composeGridToCpu({src}, 32, 32, color, GpuCompositor::ScaleQuality::Bilinear);
    QVERIFY(gpu.isValid());

    const QByteArray refG = packChannel(ref, 1);
    const QByteArray gpuG = packChannel(gpu, 1);
    const double psnr = psnrY8(reinterpret_cast<const uint8_t*>(refG.constData()), 32,
                               reinterpret_cast<const uint8_t*>(gpuG.constData()), 32, 32, 32);
    QVERIFY2(psnr >= 39.0, qPrintable(QStringLiteral("bilinear PSNR %1 dB < 39").arg(psnr)));
}

void TestGpuCompositor::bilinearGridMeetsPsnrAgainstBilinearReferenceOnWarp() {
    auto rhi = GpuRhiContext::createWarpForTest();
    if (!rhi) {
        if (warpRequiredForTest()) QFAIL("required QRhi WARP/D3D11 backend unavailable");
        QSKIP("QRhi WARP/D3D11 backend unavailable on this host");
    }
    QVERIFY(rhi->isGpuBacked());
    QVERIFY(!rhi->isNullBackend());

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    const QList<FrameHandle> frames = nonDivisibleGridFrames();
    ColorMetadata color;
    const CpuPlanes ref = bilinearGridReferenceRgba8(frames, 17, 11, color);
    QVERIFY(ref.isValid());
    const CpuPlanes gpu =
        comp->composeGridToCpu(frames, 17, 11, color, GpuCompositor::ScaleQuality::Bilinear);
    QVERIFY(gpu.isValid());

    const QByteArray refG = packChannel(ref, 1);
    const QByteArray gpuG = packChannel(gpu, 1);
    const double psnr = psnrY8(reinterpret_cast<const uint8_t*>(refG.constData()), 17,
                               reinterpret_cast<const uint8_t*>(gpuG.constData()), 17, 17, 11);
    QVERIFY2(psnr >= 35.0, qPrintable(QStringLiteral("bilinear grid PSNR %1 dB < 35").arg(psnr)));
}

void TestGpuCompositor::bilinearDiffersFromNearestOracle() {
    auto rhi = GpuRhiContext::create();
    if (!rhi || !rhi->isGpuBacked()) QSKIP("no local GPU backend on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    const FrameHandle src = gradientYuv420pHandle(8, 8);
    ColorMetadata color;
    const CpuPlanes nearest = formatcanon::referenceComposeGridRgba8({src}, 32, 32, color);
    const CpuPlanes bilinear =
        comp->composeGridToCpu({src}, 32, 32, color, GpuCompositor::ScaleQuality::Bilinear);
    QVERIFY(bilinear.isValid());
    QVERIFY2(bilinear.plane[0] != nearest.plane[0],
             "bilinear scaler must differ from the nearest-neighbor oracle");
}

void TestGpuCompositor::memoHitReusesSameGpuSurface() {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) QSKIP("QRhi Null backend unavailable on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> frames{
        solidYuv420pHandle(4, 4, 40, 60, 200),
        solidYuv420pHandle(4, 4, 80, 70, 190),
    };
    QVector<qint64> keys{1, 100, 1, 200};
    MultiviewComposite memo;
    ColorMetadata color;

    const FrameHandle a = comp->composeGridMemoized(
        frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat, keys, &memo);
    QVERIFY(!a.isNull());
    QVERIFY(memo.valid);

    const FrameHandle b = comp->composeGridMemoized(
        frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat, keys, &memo);
    QVERIFY(!b.isNull());
    QVERIFY(a.dataPtr() == b.dataPtr());
}

void TestGpuCompositor::memoMissRendersFresh() {
    auto rhi = GpuRhiContext::createNullForTest();
    if (!rhi) QSKIP("QRhi Null backend unavailable on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200)};
    MultiviewComposite memo;
    ColorMetadata color;

    const FrameHandle a = comp->composeGridMemoized(
        frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat, {1, 100}, &memo);
    const FrameHandle b = comp->composeGridMemoized(
        frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat, {1, 999}, &memo);
    QVERIFY(!a.isNull());
    QVERIFY(!b.isNull());
    QVERIFY(a.dataPtr() != b.dataPtr());
}

#ifdef __APPLE__
void TestGpuCompositor::cpuHandleUploadsToNv12Surface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    const FrameHandle source = patternedYuv420pHandle(18, 10);
    const CpuPlanes expected = source.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(expected.isValid());

    auto surface = GpuCompositor::uploadFrameToNv12SurfaceForTest(source, rhi);
    QVERIFY(surface != nullptr);
    QVERIFY(surface->isValid());
    QCOMPARE(surface->desc().format, FramePixelFormat::Nv12);
    QCOMPARE(surface->desc().width, expected.width);
    QCOMPARE(surface->desc().height, expected.height);

    FrameMetadata meta = source.metadata();
    FrameHandle uploaded = makeGpuFrameHandle(surface, rhi, meta);
    const CpuPlanes actual = uploaded.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(actual.isValid());
    QCOMPARE(actual.width, expected.width);
    QCOMPARE(actual.height, expected.height);

    const int chromaW = (expected.width + 1) / 2;
    const int chromaH = (expected.height + 1) / 2;
    compareRows(expected, actual, 0, expected.height, expected.width);
    compareRows(expected, actual, 1, chromaH, chromaW);
    compareRows(expected, actual, 2, chromaH, chromaW);
}

void TestGpuCompositor::gpuNv12HandleAliasesExistingSurface() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");

    auto surface =
        GpuCompositor::uploadFrameToNv12SurfaceForTest(patternedYuv420pHandle(16, 8), rhi);
    QVERIFY(surface != nullptr);

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = surface->desc().width;
    meta.key.height = surface->desc().height;
    FrameHandle gpuFrame = makeGpuFrameHandle(surface, rhi, meta);

    auto aliased = GpuCompositor::uploadFrameToNv12SurfaceForTest(gpuFrame, rhi);
    QCOMPARE(aliased.get(), surface.get());
}
#endif

QTEST_MAIN(TestGpuCompositor)
#include "tst_gpucompositor.moc"
