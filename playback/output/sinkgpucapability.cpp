#include "playback/output/sinkgpucapability.h"

SinkGpuCapability gpuCapabilityFor(OutputTargetKind kind) {
    switch (kind) {
    case OutputTargetKind::QtPreview:
        return SinkGpuCapability::AsyncReadbackDedupOk;
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110:
        return SinkGpuCapability::GpuNative;
    case OutputTargetKind::Ndi:
    case OutputTargetKind::Omt:
    case OutputTargetKind::Aja:
        return SinkGpuCapability::NeedsContinuousCadence;
    }
    return SinkGpuCapability::NeedsContinuousCadence;
}
