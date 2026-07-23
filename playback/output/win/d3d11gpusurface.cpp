#include "playback/output/win/d3d11gpusurface.h"

#ifdef _WIN32

#include <utility>

namespace {
std::atomic<bool> g_forceAllocFailure{false};
}

std::shared_ptr<D3D11GpuSurface>
D3D11GpuSurface::createKept(Microsoft::WRL::ComPtr<ID3D11Device> device,
                            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, UINT subresource,
                            int width, int height, uint64_t authorityEpoch) {
    if (g_forceAllocFailure.load(std::memory_order_acquire)) return nullptr;
    if (!device || !texture || width <= 0 || height <= 0 || authorityEpoch == 0) return nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (!textureDevice) return nullptr;

    Microsoft::WRL::ComPtr<IUnknown> suppliedIdentity;
    Microsoft::WRL::ComPtr<IUnknown> textureDeviceIdentity;
    if (FAILED(device.As(&suppliedIdentity)) || !suppliedIdentity ||
        FAILED(textureDevice.As(&textureDeviceIdentity)) || !textureDeviceIdentity ||
        suppliedIdentity.Get() != textureDeviceIdentity.Get()) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != UINT(width) ||
        desc.Height != UINT(height) || desc.MipLevels != 1 || desc.ArraySize == 0 ||
        subresource >= desc.ArraySize || desc.SampleDesc.Count != 1) {
        return nullptr;
    }

    auto surface = std::shared_ptr<D3D11GpuSurface>(new D3D11GpuSurface());
    surface->m_deviceIdentity = std::move(textureDeviceIdentity);
    surface->m_device = std::move(textureDevice);
    surface->m_texture = std::move(texture);
    surface->m_authorityEpoch = authorityEpoch;
    surface->m_subresource = subresource;
    surface->m_width = width;
    surface->m_height = height;
    return surface;
}

void D3D11GpuSurface::setForceAllocFailureForTest(bool force) {
    g_forceAllocFailure.store(force, std::memory_order_release);
}

#endif // _WIN32
