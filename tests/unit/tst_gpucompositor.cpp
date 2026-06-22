// GpuCompositor is the RHI grid/PGM compositor; the CPU Yuv420pCompositor stays
// the oracle + fallback. With no RHI backend create() may return nullptr; with a
// backend it must never return a partially initialized compositor.
#include <QtTest>

#include <QFile>
#include <rhi/qshader.h>

#include "playback/gpu/gpuframedata.h"
#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/formatcanon.h"
#include "playback/output/framehandle.h"

#include <utility>

class TestGpuCompositor : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void gridShadersLoadAndBuildPipeline();
    void compatGridMatchesCpuOracleExactOnNull();
    void compatGridWithinOneLsbOnLocalGpu();
    void swappedQuadrantsDifferFromOracle();
    void pgmSelectFillsOutputFromSingleSource();
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

    QVERIFY2(vert.isValid(), "grid.vert.qsb missing or not deserializable");
    QVERIFY2(frag.isValid(), "grid_nn.frag.qsb missing or not deserializable");
    QCOMPARE(vert.stage(), QShader::VertexStage);
    QCOMPARE(frag.stage(), QShader::FragmentStage);
}

void TestGpuCompositor::compatGridMatchesCpuOracleExactOnNull() {
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

void TestGpuCompositor::compatGridWithinOneLsbOnLocalGpu() {
    auto rhi = GpuRhiContext::create();
    if (!rhi || !rhi->isGpuBacked()) QSKIP("no local GPU backend on this host");

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
    if (!gpu.isValid()) QSKIP("local GPU grid render/readback unavailable on this host");
    compareRgbaWithinOneLsb(gpu, oracle);
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

QTEST_GUILESS_MAIN(TestGpuCompositor)
#include "tst_gpucompositor.moc"
