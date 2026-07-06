// SinkCapabilityProbe is the single GPU-path selector for the new I/O targets.
// DeckLink is GPU-native only when the SDK is built and the device confirms
// texture input; AJA/OMT are CPU-frame sinks that need continuous cadence.
#include <QtTest>

#include "playback/output/iotargets/sinkcapabilityprobe.h"

class TestSinkCapabilityProbe : public QObject {
    Q_OBJECT
private slots:
    void deckLinkGpuNativeWhenSdkAndDevice();
    void deckLinkFallsToCadenceWithoutGpuTextureInput();
    void deckLinkFallsToCadenceWithoutSdk();
    void ajaAlwaysContinuousCadence();
    void omtAlwaysContinuousCadence();
    void nonIoTargetsDefaultToDedupOk();
};

void TestSinkCapabilityProbe::deckLinkGpuNativeWhenSdkAndDevice() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkSdiHdmi, {},
                                           /*sdkBuilt=*/true, /*deviceGpuTextureInput=*/true),
             SinkGpuCapability::GpuNative);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkIpSt2110, {}, true, true),
             SinkGpuCapability::GpuNative);
}

void TestSinkCapabilityProbe::deckLinkFallsToCadenceWithoutGpuTextureInput() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkSdiHdmi, {}, true, false),
             SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkIpSt2110, {}, true, false),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::deckLinkFallsToCadenceWithoutSdk() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkSdiHdmi, {}, false, true),
             SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::DeckLinkIpSt2110, {}, false, true),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::ajaAlwaysContinuousCadence() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Aja, {}, true, true),
             SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Aja, {}, false, false),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::omtAlwaysContinuousCadence() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Omt, {}, true, true),
             SinkGpuCapability::NeedsContinuousCadence);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Omt, {}, false, false),
             SinkGpuCapability::NeedsContinuousCadence);
}

void TestSinkCapabilityProbe::nonIoTargetsDefaultToDedupOk() {
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::QtPreview, {}, true, true),
             SinkGpuCapability::AsyncReadbackDedupOk);
    QCOMPARE(SinkCapabilityProbe::classify(OutputTargetKind::Ndi, {}, true, true),
             SinkGpuCapability::AsyncReadbackDedupOk);
}

QTEST_GUILESS_MAIN(TestSinkCapabilityProbe)
#include "tst_sinkcapabilityprobe.moc"
