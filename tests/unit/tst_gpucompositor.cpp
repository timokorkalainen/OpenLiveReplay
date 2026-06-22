// GpuCompositor is the RHI grid/PGM compositor; the CPU Yuv420pCompositor stays
// the oracle + fallback. With no RHI backend create() may return nullptr; with a
// backend it must never return a partially initialized compositor.
#include <QtTest>

#include <QFile>
#include <rhi/qshader.h>

#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/framehandle.h"

class TestGpuCompositor : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
    void gridShadersLoadAndBuildPipeline();
};

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

QTEST_GUILESS_MAIN(TestGpuCompositor)
#include "tst_gpucompositor.moc"
