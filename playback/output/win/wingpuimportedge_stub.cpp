#include "playback/output/win/wingpuimportedge.h"

#ifndef _WIN32

WinGpuImportCapabilities probeWinGpuImport() {
    WinGpuImportCapabilities caps;
    caps.backend = QStringLiteral("none");
    caps.detail = QStringLiteral("no Windows GPU import on this platform");
    return caps;
}

struct WinGpuImportEdge::Impl {};

std::unique_ptr<WinGpuImportEdge> WinGpuImportEdge::create(QString* error) {
    if (error) *error = QStringLiteral("Windows GPU import edge unavailable on this platform");
    return nullptr;
}

WinGpuImportEdge::WinGpuImportEdge() = default;
WinGpuImportEdge::~WinGpuImportEdge() = default;

bool WinGpuImportEdge::isAvailable() const {
    return false;
}

std::optional<FrameHandle> WinGpuImportEdge::tryImport(void*, int, qint64, int, int,
                                                       std::shared_ptr<GpuFence>) {
    return std::nullopt;
}

FrameHandle WinGpuImportEdge::makeGpuFrameHandleForTest(std::shared_ptr<D3D11GpuSurface>,
                                                        FrameMetadata,
                                                        std::shared_ptr<GpuFence>) {
    return FrameHandle();
}

#endif // !_WIN32
