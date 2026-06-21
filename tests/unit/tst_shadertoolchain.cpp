// Shader-toolchain proof: the RHI spine links, a QRhi over the deterministic
// Null backend creates, and an empty offscreen frame runs headlessly. The baked
// .qsb pipeline assertion is added after the RHI helper exists.
#include <QColor>
#include <QFile>
#include <QSize>
#include <QtTest>

#include "playback/gpu/olrrhi.h"

#include <rhi/qshader.h>
#include <rhi/qrhi.h>

#include <memory>

class TestShaderToolchain : public QObject {
    Q_OBJECT
private slots:
    void nullBackendCreatesAndRunsEmptyFrame();
    void bakedPassthroughQsbLoadsAndBuildsPipeline();
};

void TestShaderToolchain::nullBackendCreatesAndRunsEmptyFrame() {
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable("OlrRhi::create failed: " + err));
    QVERIFY(rhi->rhi() != nullptr);
    QCOMPARE(rhi->rhi()->backend(), QRhi::Null);

    bool recorded = false;
    const bool ok = rhi->runOffscreenFrame(
        [&](QRhiCommandBuffer* cb) {
            QVERIFY(cb != nullptr);
            recorded = true;
        },
        &err);
    QVERIFY2(ok, qPrintable("offscreen frame failed: " + err));
    QVERIFY(recorded);
}

void TestShaderToolchain::bakedPassthroughQsbLoadsAndBuildsPipeline() {
    auto loadShader = [](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return QShader();
        }
        return QShader::fromSerialized(f.readAll());
    };

    const QShader vert = loadShader(QStringLiteral(":/olr/shaders/passthrough.vert.qsb"));
    const QShader frag = loadShader(QStringLiteral(":/olr/shaders/passthrough.frag.qsb"));
    QVERIFY2(vert.isValid(), "passthrough.vert.qsb missing or not deserializable");
    QVERIFY2(frag.isValid(), "passthrough.frag.qsb missing or not deserializable");
    QCOMPARE(vert.stage(), QShader::VertexStage);
    QCOMPARE(frag.stage(), QShader::FragmentStage);

    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable("OlrRhi::create failed: " + err));
    QRhi* r = rhi->rhi();

    std::unique_ptr<QRhiTexture> tex(
        r->newTexture(QRhiTexture::RGBA8, QSize(64, 64), 1, QRhiTexture::RenderTarget));
    QVERIFY(tex->create());

    std::unique_ptr<QRhiTextureRenderTarget> rt(r->newTextureRenderTarget(
        QRhiTextureRenderTargetDescription(QRhiColorAttachment(tex.get()))));
    std::unique_ptr<QRhiRenderPassDescriptor> rpDesc(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rpDesc.get());
    QVERIFY(rt->create());

    std::unique_ptr<QRhiShaderResourceBindings> srb(r->newShaderResourceBindings());
    QVERIFY(srb->create());

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(r->newGraphicsPipeline());
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, frag}});
    QRhiVertexInputLayout inputLayout;
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(srb.get());
    pipeline->setRenderPassDescriptor(rpDesc.get());
    QVERIFY2(pipeline->create(), "QRhiGraphicsPipeline build from baked .qsb failed");

    const bool ok = rhi->runOffscreenFrame(
        [&](QRhiCommandBuffer* cb) {
            cb->beginPass(rt.get(), QColor(0, 0, 0, 255), {1.0f, 0});
            cb->setGraphicsPipeline(pipeline.get());
            cb->setViewport(QRhiViewport(0, 0, 64, 64));
            cb->setShaderResources();
            cb->draw(3);
            cb->endPass();
        },
        &err);
    QVERIFY2(ok, qPrintable("offscreen draw failed: " + err));
}

QTEST_GUILESS_MAIN(TestShaderToolchain)
#include "tst_shadertoolchain.moc"
