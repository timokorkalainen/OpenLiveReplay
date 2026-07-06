#ifndef OLR_WIN_GPU_IMPORT_EDGE_H
#define OLR_WIN_GPU_IMPORT_EDGE_H

#include "playback/output/framehandle.h"

#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/gpubudget.h"
#endif

#include <QString>

#include <functional>
#include <memory>
#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
#include <wrl/client.h>
#endif

struct WinGpuImportCapabilities {
    bool d3d11KeepTexture = false;
    bool rhiImportable = false;
    QString backend;
    QString detail;
};

WinGpuImportCapabilities probeWinGpuImport();

#ifdef OLR_UNIT_TEST
bool winGpuImportProbeForcesHardwareDecoderForTest();
#endif

class GpuFence;
class D3D11GpuSurface;

class WinGpuImportEdge {
public:
    static std::unique_ptr<WinGpuImportEdge> create(QString* error);
    ~WinGpuImportEdge();

    WinGpuImportEdge(const WinGpuImportEdge&) = delete;
    WinGpuImportEdge& operator=(const WinGpuImportEdge&) = delete;

    std::optional<FrameHandle> tryImport(void* mfSampleOpaque, int feedIndex, qint64 ptsMs,
                                         int width, int height,
                                         std::shared_ptr<GpuFence> renderFence = nullptr);
    std::shared_ptr<D3D11GpuSurface> tryImportSurface(void* mfSampleOpaque, int width, int height);
    bool isAvailable() const;
    bool deviceLost() const;

#ifdef OLR_GPU_PIPELINE_BUILD
    static FrameHandle makeGpuFrameHandleForTest(std::shared_ptr<D3D11GpuSurface> surface,
                                                 FrameMetadata meta,
                                                 std::shared_ptr<GpuFence> renderFence = nullptr,
                                                 GpuBudgetCharge charge = {});
#else
    static FrameHandle makeGpuFrameHandleForTest(std::shared_ptr<D3D11GpuSurface> surface,
                                                 FrameMetadata meta,
                                                 std::shared_ptr<GpuFence> renderFence = nullptr);
#endif
#ifdef _WIN32
    void setImportTapForTest(std::function<void(const FrameHandle&)> tap);
    void* d3d11Device() const;
    bool decodeOneForTest(Microsoft::WRL::ComPtr<ID3D11Device> device,
                          Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12, int width, int height);
#endif

private:
    WinGpuImportEdge();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

constexpr const char* kWinRhiBackend = "d3d11";

#endif // OLR_WIN_GPU_IMPORT_EDGE_H
