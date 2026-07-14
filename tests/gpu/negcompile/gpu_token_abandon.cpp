#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpuretireregistry.h"

#if defined(OLR_NEGCOMPILE_TOKEN_CONSTRUCTION)
DeadDeviceToken forbiddenDeadDeviceTokenConstruction() {
    return DeadDeviceToken(DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, 1, 2, 3);
}
#elif defined(OLR_NEGCOMPILE_DIRECT_ABANDON)
qsizetype forbiddenDirectTokenAbandon(GpuRetireRegistry& registry, const DeadDeviceToken& token) {
    return registry.abandonAllNoWait(token);
}
#else
#error "Select one exact token capability probe"
#endif
