#include "playback/gpu/gpudevicelossmonitor.h"

struct DxgiDeviceLossAuthority {
    static uint64_t forge() {
        return GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason);
    }
};

uint64_t forbiddenTokenAuthoritySpoof() {
    return DxgiDeviceLossAuthority::forge();
}
