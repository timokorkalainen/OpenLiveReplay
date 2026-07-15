#include "playback/gpu/gpufence.h"

#ifdef _WIN32

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/output/win/d3dfence.h"

#include <QString>

#include <d3d11.h>
#include <atomic>
#include <memory>
#include <wrl/client.h>

namespace {

std::atomic<uint64_t> nextFenceInstanceId{1};

uintptr_t d3d11DeviceDomainId(ID3D11Device* device) {
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
    return reinterpret_cast<uintptr_t>(identity.Get());
}

class D3D11GpuFence final : public GpuFence {
public:
    D3D11GpuFence(std::unique_ptr<D3DFence> fence, ID3D11Device* device, uint64_t authorityEpoch)
        : GpuFence(d3d11DeviceDomainId(device), authorityEpoch), m_fence(std::move(fence)),
          m_device(device) {
        device->GetImmediateContext(&m_context);
        device->QueryInterface(IID_PPV_ARGS(&m_deviceIdentity));
    }

    ~D3D11GpuFence() override {
        if (m_context) m_context->Release();
    }

    uint64_t signal() override { return m_context ? m_fence->signal(m_context) : 0; }
    bool wait(uint64_t value, int timeoutMs) override { return m_fence->wait(value, timeoutMs); }
    uint64_t completedValue() const override { return m_fence->completedValue(); }
    uintptr_t deviceDomainId() const override {
        return reinterpret_cast<uintptr_t>(m_deviceIdentity.Get());
    }

protected:
    bool isCompatibleWithNativeHandle(void* handle) const override {
        auto* texture = static_cast<ID3D11Texture2D*>(handle);
        if (!texture || !m_deviceIdentity) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<IUnknown> identity;
        texture->GetDevice(&device);
        return device && SUCCEEDED(device.As(&identity)) &&
               identity.Get() == m_deviceIdentity.Get();
    }

private:
    std::unique_ptr<D3DFence> m_fence;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    ID3D11DeviceContext* m_context = nullptr;
    Microsoft::WRL::ComPtr<IUnknown> m_deviceIdentity;
};

} // namespace

GpuFence::GpuFence(uintptr_t deviceDomainId, uint64_t authorityEpoch)
    : m_identity{gpuSubmissionDetail::takeMonotonicInstanceId(nextFenceInstanceId), deviceDomainId,
                 authorityEpoch != 0
                     ? authorityEpoch
                     : GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch()},
      m_ticketAuthorityKey(makeTicketAuthorityKey(m_identity, this)) {}

GpuFence::~GpuFence() = default;

uint64_t GpuFence::currentGpuGeneration() noexcept {
    return GpuGenerationCounter::instance().current();
}

bool GpuFence::validatesRetirement(const GpuRetirementTicket& ticket,
                                   const GpuSurfaceCompatibility& surface) const noexcept {
    return ticket.m_fence.get() == this && ticket.m_identity == identity() && ticket.m_value != 0 &&
           gpuSubmissionDetail::matchesSurfaceEvidence(
               surface, ticket.m_identity, ticket.m_gpuGeneration, currentGpuGeneration()) &&
           ticket.m_authoritySeal ==
               sealTicket(ticket.m_identity, ticket.m_gpuGeneration, ticket.m_value);
}

std::shared_ptr<GpuFence> makeD3D11GpuFence(void* d3d11Device, uint64_t authorityEpoch) {
    auto* device = static_cast<ID3D11Device*>(d3d11Device);
    if (!device || authorityEpoch == 0) return nullptr;

    QString error;
    auto fence = D3DFence::create(device, &error);
    if (!fence) return nullptr;
    return std::make_shared<D3D11GpuFence>(std::move(fence), device, authorityEpoch);
}

std::shared_ptr<GpuFence> GpuFence::create() {
    return nullptr;
}

#endif // _WIN32
