#include "playback/gpu/gpufence.h"

#ifdef _WIN32

#include "playback/output/win/d3dfence.h"

#include <QString>

#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

GpuFence::~GpuFence() = default;

namespace {

class D3D11GpuFence final : public GpuFence {
public:
    D3D11GpuFence(std::unique_ptr<D3DFence> fence, ID3D11Device* device)
        : m_fence(std::move(fence)) {
        device->GetImmediateContext(&m_context);
        device->QueryInterface(IID_PPV_ARGS(&m_deviceIdentity));
    }

    ~D3D11GpuFence() override {
        if (m_context) m_context->Release();
    }

    uint64_t signal() override { return m_context ? m_fence->signal(m_context) : 0; }
    bool wait(uint64_t value, int timeoutMs) override { return m_fence->wait(value, timeoutMs); }
    uint64_t completedValue() const override { return m_fence->completedValue(); }

protected:
    bool isCompatibleWithNativeHandle(void* nativeHandle) const override {
        auto* texture = static_cast<ID3D11Texture2D*>(nativeHandle);
        if (!texture || !m_deviceIdentity) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<IUnknown> identity;
        texture->GetDevice(&device);
        return device && SUCCEEDED(device.As(&identity)) &&
               identity.Get() == m_deviceIdentity.Get();
    }

private:
    std::unique_ptr<D3DFence> m_fence;
    ID3D11DeviceContext* m_context = nullptr;
    Microsoft::WRL::ComPtr<IUnknown> m_deviceIdentity;
};

} // namespace

std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device) {
    auto* device = static_cast<ID3D11Device*>(d3d11Device);
    if (!device) return nullptr;

    QString error;
    auto fence = D3DFence::create(device, &error);
    if (!fence) return nullptr;
    return std::make_shared<D3D11GpuFence>(std::move(fence), device);
}

std::shared_ptr<GpuFence> GpuFence::create() {
    return nullptr;
}

#endif // _WIN32
