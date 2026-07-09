#include "playback/output/iotargets/sinkcapabilityprobe.h"

#include <QtGlobal>

SinkGpuCapability SinkCapabilityProbe::classify(OutputTargetKind kind, const QVariantMap& settings,
                                                bool sdkBuilt, bool deviceGpuTextureInput) {
    Q_UNUSED(settings);

    switch (kind) {
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110:
        return (sdkBuilt && deviceGpuTextureInput) ? SinkGpuCapability::GpuNative
                                                   : SinkGpuCapability::NeedsContinuousCadence;
    case OutputTargetKind::Aja:
    case OutputTargetKind::Omt:
        return SinkGpuCapability::NeedsContinuousCadence;
    case OutputTargetKind::QtPreview:
    case OutputTargetKind::Ndi:
        break;
    }

    return SinkGpuCapability::AsyncReadbackDedupOk;
}
