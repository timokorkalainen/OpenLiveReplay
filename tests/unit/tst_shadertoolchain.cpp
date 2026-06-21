// Shader-toolchain proof: the RHI spine links, a QRhi over the deterministic
// Null backend creates, and an empty offscreen frame runs headlessly. The baked
// .qsb pipeline assertion is added after the RHI helper exists.
#include <QtTest>

#include "playback/gpu/olrrhi.h"

#include <rhi/qrhi.h>

class TestShaderToolchain : public QObject {
    Q_OBJECT
private slots:
    void nullBackendCreatesAndRunsEmptyFrame();
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

QTEST_GUILESS_MAIN(TestShaderToolchain)
#include "tst_shadertoolchain.moc"
