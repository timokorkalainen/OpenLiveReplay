#include "playback/output/win/wingpuimportedge.h"

void* forbiddenImportEdgeDeviceEscape(const WinGpuImportEdge& edge) {
    return edge.d3d11Device();
}
