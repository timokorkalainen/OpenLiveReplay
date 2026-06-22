#include "playback/gpu/gpurhicontext.h"

#ifdef _WIN32

#include "playback/gpu/gpufence.h"

#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <d3d10_1.h>
#include <d3d11.h>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

enum class D3DDeviceKind { Hardware, Warp };

bool createD3D11Device(D3DDeviceKind kind, ComPtr<ID3D11Device>* device,
                       ComPtr<ID3D11DeviceContext>* context) {
    if (!device || !context) return false;

    const std::array<D3D_FEATURE_LEVEL, 4> levels{D3D_FEATURE_LEVEL_11_1,
                                                  D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1,
                                                  D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE driverType =
        kind == D3DDeviceKind::Warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr = D3D11CreateDevice(nullptr, driverType, nullptr, flags, levels.data(),
                                   UINT(levels.size()), D3D11_SDK_VERSION, &*device, &created,
                                   &*context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, driverType, nullptr, flags, levels.data() + 1,
                               UINT(levels.size() - 1), D3D11_SDK_VERSION, &*device, &created,
                               &*context);
    }
    if (FAILED(hr) || !device->Get() || !context->Get()) return false;

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED((*device).As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    return true;
}

class D3DRenderThread final : public QThread {
public:
    explicit D3DRenderThread(D3DDeviceKind kind) : m_kind(kind) {}

    QRhi* rhi = nullptr;

    void run() override {
        QRhiD3D11InitParams params;
        QRhiD3D11NativeHandles handles;
        if (createD3D11Device(m_kind, &m_device, &m_context)) {
            handles.dev = m_device.Get();
            handles.context = m_context.Get();
            rhi = QRhi::create(QRhi::D3D11, &params, {}, &handles);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_ready = true;
            m_cond.wakeAll();
        }

        while (true) {
            std::function<void()> job;
            {
                QMutexLocker lock(&m_mutex);
                while (m_jobs.isEmpty() && !m_stop) {
                    m_cond.wait(&m_mutex);
                }
                if (m_stop && m_jobs.isEmpty()) break;
                job = m_jobs.takeFirst();
            }
            job();
        }

        delete rhi;
        rhi = nullptr;
        m_context.Reset();
        m_device.Reset();
    }

    bool waitReady() {
        QMutexLocker lock(&m_mutex);
        while (!m_ready) {
            m_cond.wait(&m_mutex);
        }
        return rhi != nullptr;
    }

    bool invoke(std::function<void()> job) {
        QMutexLocker lock(&m_mutex);
        if (m_stop) return false;

        bool done = false;
        m_jobs.append([&] {
            job();
            QMutexLocker doneLock(&m_mutex);
            done = true;
            m_cond.wakeAll();
        });
        m_cond.wakeAll();
        while (!done) {
            m_cond.wait(&m_mutex);
        }
        return true;
    }

    void requestStop() {
        QMutexLocker lock(&m_mutex);
        m_stop = true;
        m_cond.wakeAll();
    }

private:
    D3DDeviceKind m_kind = D3DDeviceKind::Hardware;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    QMutex m_mutex;
    QWaitCondition m_cond;
    QList<std::function<void()>> m_jobs;
    bool m_ready = false;
    bool m_stop = false;
};

} // namespace

class GpuRhiContext::Impl {
public:
    explicit Impl(D3DDeviceKind kind) : thread(kind) {}

    D3DRenderThread thread;
    bool valid = false;
};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GpuRhiContext::~GpuRhiContext() {
    if (!m_impl) return;
    m_impl->thread.requestStop();
    m_impl->thread.wait();
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
    auto impl = std::make_unique<Impl>(D3DDeviceKind::Hardware);
    impl->thread.start();
    impl->valid = impl->thread.waitReady();
    if (!impl->valid) {
        impl->thread.requestStop();
        impl->thread.wait();
        return nullptr;
    }
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::move(impl)));
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::createNullForTest() {
    return nullptr;
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::createWarpForTest() {
    auto impl = std::make_unique<Impl>(D3DDeviceKind::Warp);
    impl->thread.start();
    impl->valid = impl->thread.waitReady();
    if (!impl->valid) {
        impl->thread.requestStop();
        impl->thread.wait();
        return nullptr;
    }
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::move(impl)));
}

bool GpuRhiContext::isValid() const {
    return m_impl && m_impl->valid;
}

bool GpuRhiContext::isNullBackend() const {
    return false;
}

bool GpuRhiContext::invokeOnRenderThread(const std::function<void(QRhi*)>& job) const {
    if (!m_impl || !m_impl->valid || !job) return false;
    return m_impl->thread.invoke([&] { job(m_impl->thread.rhi); });
}

CpuPlanes GpuRhiContext::importAndReadback(const std::shared_ptr<GpuSurface>&, FramePixelFormat) {
    return CpuPlanes{};
}

std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    if (!m_impl || !m_impl->valid) return nullptr;

    std::shared_ptr<GpuFence> fence;
    const bool invoked = m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (!rhi) return;
        const auto* nativeHandles =
            static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        fence = nativeHandles ? makeD3D11GpuFence(nativeHandles->dev) : nullptr;
    });
    return invoked ? fence : nullptr;
}

#endif // _WIN32
