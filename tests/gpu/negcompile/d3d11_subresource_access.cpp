#include "playback/output/win/d3d11gpusurface.h"

UINT forbiddenSubresourceAccess(const D3D11GpuSurface& surface) {
    return surface.subresource();
}
