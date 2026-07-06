#ifndef IOTARGETSINKFACTORY_H
#define IOTARGETSINKFACTORY_H

#include "playback/output/outputsink.h"

#include <memory>

class GpuFence;
class IDeckLinkSenderBackend;
class SharedGpuReadbackCache;

std::unique_ptr<IOutputSink>
makeIoTargetSink(const OutputTargetAssignment& assignment, FrameRate rate,
                 std::shared_ptr<GpuFence> renderFence = nullptr,
                 std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks = nullptr);

#ifdef OLR_UNIT_TEST
std::unique_ptr<IOutputSink>
makeDeckLinkIoTargetSinkForTest(OutputTargetKind kind, IDeckLinkSenderBackend* backend,
                                std::shared_ptr<GpuFence> renderFence = nullptr,
                                std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks = nullptr);
#endif

#endif // IOTARGETSINKFACTORY_H
