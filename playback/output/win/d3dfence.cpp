#include "playback/output/win/d3dfence.h"

#ifdef _WIN32

#include "playback/output/win/wingpuimportedge.h"

#include <QtGlobal>

#if defined(__has_include)
#if __has_include(<d3d11_4.h>)
#include <d3d11_4.h>
#define OLR_HAS_D3D11_FENCE 1
#endif
#endif

#include <mutex>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct D3DFence::Impl {
#ifdef OLR_HAS_D3D11_FENCE
    ComPtr<ID3D11Fence> fence;
#endif
    std::mutex signalMutex;
    uint64_t nextValue = 0;
};

D3DFence::D3DFence() : m_impl(std::make_unique<Impl>()) {}
D3DFence::~D3DFence() = default;

std::unique_ptr<D3DFence> D3DFence::create(ID3D11Device* device, QString* error) {
    if (!device) {
        if (error) *error = QStringLiteral("D3DFence requires an ID3D11Device");
        return nullptr;
    }
    if (QString::fromLatin1(kWinRhiBackend) != QStringLiteral("d3d11")) {
        if (error) *error = QStringLiteral("D3DFence backend mismatch");
        return nullptr;
    }
#ifndef OLR_HAS_D3D11_FENCE
    if (error) *error = QStringLiteral("ID3D11Fence headers unavailable");
    return nullptr;
#else
    ComPtr<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || !device5) {
        if (error) *error = QStringLiteral("ID3D11Device5 unavailable on this host");
        return nullptr;
    }

    auto out = std::unique_ptr<D3DFence>(new D3DFence());
    if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&out->m_impl->fence))) ||
        !out->m_impl->fence) {
        if (error) *error = QStringLiteral("ID3D11Fence creation failed");
        return nullptr;
    }
    return out;
#endif
}

uint64_t D3DFence::signal(ID3D11DeviceContext* context) {
#ifndef OLR_HAS_D3D11_FENCE
    Q_UNUSED(context);
    return 0;
#else
    if (!context || !m_impl || !m_impl->fence) return 0;
    std::lock_guard<std::mutex> lock(m_impl->signalMutex);
    ComPtr<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))) || !context4) return 0;
    const uint64_t value = ++m_impl->nextValue;
    if (FAILED(context4->Signal(m_impl->fence.Get(), value))) return 0;
    return value;
#endif
}

bool D3DFence::wait(uint64_t value, int timeoutMs) {
#ifndef OLR_HAS_D3D11_FENCE
    Q_UNUSED(value);
    Q_UNUSED(timeoutMs);
    return false;
#else
    if (!m_impl || !m_impl->fence) return false;
    if (m_impl->fence->GetCompletedValue() >= value) return true;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const HRESULT hr = m_impl->fence->SetEventOnCompletion(value, event);
    if (FAILED(hr)) {
        CloseHandle(event);
        return false;
    }

    const DWORD waitMs = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
    const DWORD result = WaitForSingleObject(event, waitMs);
    CloseHandle(event);
    return result == WAIT_OBJECT_0 && m_impl->fence->GetCompletedValue() >= value;
#endif
}

uint64_t D3DFence::completedValue() const {
#ifndef OLR_HAS_D3D11_FENCE
    return 0;
#else
    return (m_impl && m_impl->fence) ? m_impl->fence->GetCompletedValue() : 0;
#endif
}

#endif // _WIN32
