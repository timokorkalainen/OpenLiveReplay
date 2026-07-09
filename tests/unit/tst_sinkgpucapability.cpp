#include <QtTest>

#include "playback/output/outputtypes.h"
#include "playback/output/sinkgpucapability.h"

class TestSinkGpuCapability : public QObject {
    Q_OBJECT

private slots:
    void previewDedups();
    void ndiNeedsContinuousCadence();
    void ajaAndOmtNeedContinuousCadence();
    void decklinkIsGpuNative();
};

void TestSinkGpuCapability::previewDedups() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::QtPreview),
             SinkGpuCapability::AsyncReadbackDedupOk);
}

void TestSinkGpuCapability::ndiNeedsContinuousCadence() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::Ndi), SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkGpuCapability::ajaAndOmtNeedContinuousCadence() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::Aja), SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::Omt), SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkGpuCapability::decklinkIsGpuNative() {
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::DeckLinkSdiHdmi), SinkGpuCapability::GpuNative);
    QCOMPARE(gpuCapabilityFor(OutputTargetKind::DeckLinkIpSt2110), SinkGpuCapability::GpuNative);
}

QTEST_MAIN(TestSinkGpuCapability)
#include "tst_sinkgpucapability.moc"
