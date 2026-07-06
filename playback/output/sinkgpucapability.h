#ifndef SINKGPUCAPABILITY_H
#define SINKGPUCAPABILITY_H

#include "playback/output/outputtypes.h"

enum class SinkGpuCapability {
    GpuNative,
    AsyncReadbackDedupOk,
    NeedsContinuousCadence,
};

SinkGpuCapability gpuCapabilityFor(OutputTargetKind kind);

#endif // SINKGPUCAPABILITY_H
