#ifndef OLR_D3D11_GPU_SURFACE_H
#define OLR_D3D11_GPU_SURFACE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "playback/gpu/gpusurface.h"

#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

class D3D11GpuSurface : public GpuSurface {
public:
    static std::shared_ptr<D3D11GpuSurface>
    createKept(Microsoft::WRL::ComPtr<ID3D11Device> device,
               Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, UINT subresource, int width,
               int height);

    GpuSurfaceDesc desc() const override {
        return GpuSurfaceDesc{FramePixelFormat::Nv12, m_width, m_height,
                              qint64(m_width) * qint64(m_height) * 3 / 2};
    }
    bool isValid() const override { return m_texture != nullptr; }

    void retainUntilFenceRetired(uint64_t fenceValue) override;
    uint64_t pendingFenceValue() const override {
        return m_pendingFence.load(std::memory_order_acquire);
    }

    static void setForceAllocFailureForTest(bool force);
#ifdef OLR_UNIT_TEST
    bool aliasesTextureForTest(ID3D11Texture2D* texture) const {
        return m_texture.Get() == texture;
    }
#endif

protected:
    // Lease-gated, mirroring the base (gpusurface.h). Kept protected on the
    // derived type too so a D3D11GpuSurface* cannot re-widen handle access.
    void* nativeHandle() const override { return m_texture.Get(); }
    std::shared_ptr<void> retainNativeHandle() const override {
        ID3D11Texture2D* texture = m_texture.Get();
        if (!texture) return {};
        texture->AddRef();
        return std::shared_ptr<void>(
            texture, [](void* value) { static_cast<ID3D11Texture2D*>(value)->Release(); });
    }
    uint32_t nativeSubresource() const override { return m_subresource; }

private:
    D3D11GpuSurface() = default;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    UINT m_subresource = 0;
    int m_width = 0;
    int m_height = 0;
    std::atomic<uint64_t> m_pendingFence{0};
};

#endif // _WIN32

#endif // OLR_D3D11_GPU_SURFACE_H
