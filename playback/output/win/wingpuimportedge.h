#ifndef OLR_WIN_GPU_IMPORT_EDGE_H
#define OLR_WIN_GPU_IMPORT_EDGE_H

#include "playback/output/framehandle.h"
#ifdef OLR_GPU_PIPELINE_BUILD
#include "playback/gpu/gpubudget.h"
#endif

#include <QString>

#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>

class QSemaphore;

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
    static std::shared_ptr<GpuFence>
    createFenceForSurface(const std::shared_ptr<D3D11GpuSurface>& surface);
    std::shared_ptr<GpuFence> createFence() const;
    bool isAvailable() const;
    bool deviceLost() const;

    static FrameHandle makeGpuFrameHandleForTest(std::shared_ptr<D3D11GpuSurface> surface,
                                                 FrameMetadata meta,
                                                 std::shared_ptr<GpuFence> renderFence = nullptr,
#ifdef OLR_GPU_PIPELINE_BUILD
                                                 GpuBudgetCharge charge = {},
#else
                                                 std::nullptr_t charge = nullptr,
#endif
                                                 uint64_t* submittedFenceValue = nullptr);
#ifdef OLR_GPU_PIPELINE_BUILD
#ifdef OLR_UNIT_TEST
    static FrameHandle makeGpuFrameHandleWithCachedCpuForTest(
        std::shared_ptr<D3D11GpuSurface> surface, FrameMetadata meta,
        std::shared_ptr<GpuFence> renderFence, GpuBudgetCharge charge,
        uint64_t* submittedFenceValue, CpuPlanes cachedCpu);
#endif
#endif
#ifdef _WIN32
    bool pollDeviceLossFor(int timeoutMs) const;
    static std::unique_ptr<WinGpuImportEdge> createUnavailableForTest();
    static void resetDeviceLossPollCountForTest() noexcept;
    static int deviceLossPollCountForTest() noexcept;
#ifdef OLR_UNIT_TEST
    bool observeDeviceRemovedForTest(HRESULT reason, uint64_t deviceAuthorityEpoch,
                                     uintptr_t deviceDomainId);
    bool deviceLostStickyForTest() const noexcept;
    void blockNextDeviceLossPollForTest(QSemaphore* entered, QSemaphore* release) noexcept;
#endif
    void setImportTapForTest(std::function<void(const FrameHandle&)> tap);
    bool acceptsD3D11DeviceForTest(void* device) const;
    bool decodeOneForTest(Microsoft::WRL::ComPtr<ID3D11Device> device,
                          Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12, int width, int height);
#endif

private:
    WinGpuImportEdge();
#ifdef _WIN32
    static uint64_t publishDeviceRemovedForMonitor(HRESULT reason, uint64_t deviceAuthorityEpoch,
                                                   uintptr_t deviceDomainId);
#endif

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

constexpr const char* kWinRhiBackend = "d3d11";

#endif // OLR_WIN_GPU_IMPORT_EDGE_H
