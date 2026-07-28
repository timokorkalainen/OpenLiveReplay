#include "playback/output/win/d3d11gpusurface.h"

ID3D11Device* forbiddenDeviceAccess(const D3D11GpuSurface& surface) {
    return surface.device();
}
