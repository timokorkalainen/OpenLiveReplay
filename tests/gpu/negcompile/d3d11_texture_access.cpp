#include "playback/output/win/d3d11gpusurface.h"

ID3D11Texture2D* forbiddenTextureAccess(const D3D11GpuSurface& surface) {
    return surface.texture();
}
