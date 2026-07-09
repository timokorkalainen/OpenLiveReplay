// OlrRhi exposes a device-loss poll. The Null backend is never spontaneously
// lost; injectDeviceLostForTest() is the deterministic CI loss source. Once
// injected, deviceLost() latches true (recovery is a fresh create()).
#include <QtTest>

#include "playback/gpu/olrrhi.h"

class TestOlrRhi : public QObject {
    Q_OBJECT
private slots:
    void nullBackendStartsNotLost();
    void injectDeviceLostLatchesTrue();
    void freshInstanceStartsNotLostAfterPriorInjectedLoss();
};

void TestOlrRhi::nullBackendStartsNotLost() {
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable(err));
    QVERIFY(!rhi->deviceLost());
}

void TestOlrRhi::injectDeviceLostLatchesTrue() {
    QString err;
    auto rhi = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(rhi != nullptr, qPrintable(err));
    QVERIFY(!rhi->deviceLost());
    rhi->injectDeviceLostForTest();
    QVERIFY(rhi->deviceLost());
    rhi->injectDeviceLostForTest(); // idempotent
    QVERIFY(rhi->deviceLost());
}

void TestOlrRhi::freshInstanceStartsNotLostAfterPriorInjectedLoss() {
    QString err;
    auto lost = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(lost != nullptr, qPrintable(err));
    lost->injectDeviceLostForTest();
    QVERIFY(lost->deviceLost());

    auto fresh = OlrRhi::create(OlrRhi::Backend::Null, &err);
    QVERIFY2(fresh != nullptr, qPrintable(err));
    QVERIFY(!fresh->deviceLost());
}

QTEST_GUILESS_MAIN(TestOlrRhi)
#include "tst_olrrhi.moc"
