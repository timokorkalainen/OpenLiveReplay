#ifndef SINKCAPABILITYPROBE_H
#define SINKCAPABILITYPROBE_H

#include "playback/output/outputtypes.h"
#include "playback/output/sinkgpucapability.h"

#include <QVariantMap>

// The single GPU-path selector for new I/O targets. Pure function: SDK-build
// and device-capability facts are supplied by backend runtime probes.
class SinkCapabilityProbe {
public:
    static SinkGpuCapability classify(OutputTargetKind kind, const QVariantMap& settings,
                                      bool sdkBuilt, bool deviceGpuTextureInput);
};

#endif // SINKCAPABILITYPROBE_H
