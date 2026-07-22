#include "playback/gpu/gpurhicontext.h"

#ifdef _WIN32

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpusurfacelease.h"

#include <QList>
#ifdef OLR_UNIT_TEST
#include <QSemaphore>
#endif
#include <QThread>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <atomic>
#include <d3d10_1.h>
#include <d3d11.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// The ONLY definition of the DXGI mint (friend of DeadDeviceToken). Global scope so
// it matches the friend + namespace-scope declaration in gpusurfacelease.h — an
// anonymous-namespace copy would be a different, non-friend function. Reached only
// from the driver-authoritative GetDeviceRemovedReason() failure branch below, so no
// other TU can construct a DeadDeviceToken from Windows.
namespace {

#ifdef OLR_UNIT_TEST
std::atomic<uint64_t> quarantinedContextCount{0};
#endif

enum class D3DDeviceKind { Hardware, Warp };

uintptr_t deviceDomainId(ID3D11Device* device) {
    ComPtr<IUnknown> identity;
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&identity)))
               ? reinterpret_cast<uintptr_t>(identity.Get())
               : 0;
}

bool createD3D11Device(D3DDeviceKind kind, ComPtr<ID3D11Device>* device,
                       ComPtr<ID3D11DeviceContext>* context) {
    if (!device || !context) return false;

    const std::array<D3D_FEATURE_LEVEL, 4> levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const D3D_DRIVER_TYPE driverType =
        kind == D3DDeviceKind::Warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_10_0;

    HRESULT hr =
        D3D11CreateDevice(nullptr, driverType, nullptr, flags, levels.data(), UINT(levels.size()),
                          D3D11_SDK_VERSION, &*device, &created, &*context);
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
            QRhi* createdRhi = QRhi::create(QRhi::D3D11, &params, {}, &handles);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                rhi = createdRhi;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready = true;
        }
        m_cond.notify_all();

        while (true) {
            QueuedJob job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cond.wait(lock, [&] { return !m_jobs.isEmpty() || m_stop; });
                if (m_stop && m_jobs.isEmpty()) break;
                job = m_jobs.takeFirst();
            }
            job.run();
        }

        if (!m_abandonCleanup.load(std::memory_order_acquire)) {
            delete rhi;
            rhi = nullptr;
            m_context.Reset();
            m_device.Reset();
        }
    }

    bool waitReady() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond.wait(lock, [&] { return m_ready; });
        return rhi != nullptr;
    }

    bool invoke(std::function<void()> job, int timeoutMs = -1) {
        if (QThread::currentThread() == this) {
            try {
                job();
                return true;
            } catch (...) {
                return false;
            }
        }
        struct InvokeState {
            std::mutex mutex;
            std::condition_variable finished;
            bool done = false;
            bool succeeded = false;
        };
        const auto state = std::make_shared<InvokeState>();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop) return false;
            m_jobs.append({[state, job = std::move(job)] {
                               bool succeeded = false;
                               try {
                                   job();
                                   succeeded = true;
                               } catch (...) {
                               }
                               {
                                   std::lock_guard<std::mutex> doneLock(state->mutex);
                                   state->succeeded = succeeded;
                                   state->done = true;
                               }
                               state->finished.notify_all();
                           },
                           [state] {
                               {
                                   std::lock_guard<std::mutex> doneLock(state->mutex);
                                   state->done = true;
                                   state->succeeded = false;
                               }
                               state->finished.notify_all();
                           }});
        }
        m_cond.notify_all();
        std::unique_lock<std::mutex> doneLock(state->mutex);
        if (timeoutMs < 0) {
            state->finished.wait(doneLock, [&] { return state->done; });
        } else if (!state->finished.wait_for(doneLock, std::chrono::milliseconds(timeoutMs),
                                             [&] { return state->done; })) {
            return false;
        }
        return state->succeeded;
    }

    bool enqueue(std::function<void()> job) {
        if (!job) return false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop) return false;
            m_jobs.append({std::move(job), {}});
        }
        m_cond.notify_all();
        return true;
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cond.notify_all();
    }

    void quarantine() {
        QList<QueuedJob> cancelled;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_abandonCleanup.store(true, std::memory_order_release);
            m_stop = true;
            cancelled.swap(m_jobs);
        }
        for (QueuedJob& job : cancelled)
            if (job.cancel) job.cancel();
        m_cond.notify_all();
    }

private:
    struct QueuedJob {
        std::function<void()> run;
        std::function<void()> cancel;
    };

    D3DDeviceKind m_kind = D3DDeviceKind::Hardware;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    QList<QueuedJob> m_jobs;
    std::atomic<bool> m_abandonCleanup{false};
    bool m_ready = false;
    bool m_stop = false;
};

} // namespace

class GpuRhiContext::Impl {
public:
    explicit Impl(D3DDeviceKind kind) : thread(kind) {}

    D3DRenderThread thread;
    bool valid = false;
    uint64_t deviceAuthorityEpoch = 0;
    std::atomic<bool> deviceLost{false};
#ifdef OLR_UNIT_TEST
    std::atomic<bool> pollOnlyDeviceLost{false};
#endif
};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl,
                             std::function<std::shared_ptr<GpuFence>()> readbackFenceFactory,
                             std::shared_ptr<std::atomic<int>> injectedFactoryCalls)
    : m_impl(std::move(impl)) {
    try {
#ifdef OLR_UNIT_TEST
        ++m_readbackFenceInitializationAttemptsForTest;
        m_injectedReadbackFenceFactoryCallsForTest = std::move(injectedFactoryCalls);
#else
        (void) injectedFactoryCalls;
#endif
        m_readbackFence = readbackFenceFactory ? readbackFenceFactory() : createFence();
    } catch (...) {
    }
}

GpuRhiContext::~GpuRhiContext() {
    if (!m_impl) return;
    std::unique_ptr<Impl> retiring = std::move(m_impl);
    retiring->thread.requestStop();
    if (retiring->thread.wait(100)) return;
    // A wedged driver call cannot be joined safely within the recovery deadline.
    // Quarantine this bounded cold-path carrier for process lifetime: pending
    // invokes are cancelled and the render thread skips Qt/COM destruction if it
    // eventually returns, avoiding wrong-affinity cleanup or post-QCoreApplication
    // teardown. The alternative would be destroying a live QThread.
    retiring->thread.quarantine();
#ifdef OLR_UNIT_TEST
    quarantinedContextCount.fetch_add(1, std::memory_order_acq_rel);
#endif
    (void) retiring.release();
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
    auto impl = std::make_unique<Impl>(D3DDeviceKind::Hardware);
    impl->deviceAuthorityEpoch = GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
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
    impl->deviceAuthorityEpoch = GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    impl->thread.start();
    impl->valid = impl->thread.waitReady();
    if (!impl->valid) {
        impl->thread.requestStop();
        impl->thread.wait();
        return nullptr;
    }
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::move(impl)));
}

#ifdef OLR_UNIT_TEST
std::shared_ptr<GpuRhiContext> GpuRhiContext::createInvalidForTest() {
    return std::shared_ptr<GpuRhiContext>(
        new GpuRhiContext(std::make_unique<Impl>(D3DDeviceKind::Warp)));
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::createReadbackFenceFailureForTest() {
    auto impl = std::make_unique<Impl>(D3DDeviceKind::Warp);
    impl->deviceAuthorityEpoch = GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
    impl->thread.start();
    impl->valid = impl->thread.waitReady();
    if (!impl->valid) {
        impl->thread.requestStop();
        impl->thread.wait();
        return nullptr;
    }
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto failingFactory = [calls] {
        calls->fetch_add(1, std::memory_order_acq_rel);
        return std::shared_ptr<GpuFence>{};
    };
    return std::shared_ptr<GpuRhiContext>(
        new GpuRhiContext(std::move(impl), std::move(failingFactory), std::move(calls)));
}

int GpuRhiContext::rhiReadbackCountForTest() const {
    return 0;
}

bool GpuRhiContext::queueBlockingRenderJobForTest(const std::shared_ptr<QSemaphore>& entered,
                                                  const std::shared_ptr<QSemaphore>& release,
                                                  const std::shared_ptr<QSemaphore>& exited) {
    if (!m_impl || !m_impl->valid || !entered || !release || !exited) return false;
    return m_impl->thread.enqueue([entered, release, exited] {
        entered->release();
        release->acquire();
        exited->release();
    });
}

uint64_t GpuRhiContext::quarantinedContextCountForTest() {
    return quarantinedContextCount.load(std::memory_order_acquire);
}

void GpuRhiContext::injectPollOnlyDeviceLostForTest() {
    if (m_impl) m_impl->pollOnlyDeviceLost.store(true, std::memory_order_release);
}
#endif

bool GpuRhiContext::isValid() const {
    return m_impl && m_impl->valid;
}

bool GpuRhiContext::isNullBackend() const {
    return false;
}

bool GpuRhiContext::invokeOnRenderThread(const std::function<void(QRhi*)>& job) const {
    const auto keepAlive = weak_from_this().lock();
    if (!keepAlive || !m_impl || !m_impl->valid || !job) return false;
    return m_impl->thread.invoke([&] { job(m_impl->thread.rhi); });
}

void GpuRhiContext::presentOnMainThread(const std::function<void()>& block) {
    if (block) block();
}

bool GpuRhiContext::deviceLost() const {
    return m_impl && m_impl->deviceLost.load(std::memory_order_acquire);
}

bool GpuRhiContext::pollDeviceLoss() const {
    if (!m_impl || !m_impl->valid) return false;
    if (m_impl->deviceLost.load(std::memory_order_acquire)) return true;
    const uint64_t authority = m_impl->deviceAuthorityEpoch;
    Impl* const impl = m_impl.get();
    const bool polled = impl->thread.invoke(
        [impl, authority] {
            QRhi* rhi = impl->thread.rhi;
            const auto* handles =
                rhi ? static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles()) : nullptr;
            auto* device = handles ? static_cast<ID3D11Device*>(handles->dev) : nullptr;
            const HRESULT reason = device ? device->GetDeviceRemovedReason() : HRESULT(S_OK);
#ifdef OLR_UNIT_TEST
            const bool injected =
                impl->pollOnlyDeviceLost.exchange(false, std::memory_order_acq_rel);
#else
            constexpr bool injected = false;
#endif
            if (!device || (!FAILED(reason) && !injected)) return;
            if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                    DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, authority,
                    deviceDomainId(device)) != 0)
                impl->deviceLost.store(true, std::memory_order_release);
        },
        50);
    return polled && impl->deviceLost.load(std::memory_order_acquire);
}

void GpuRhiContext::injectDeviceLostForTest() {
    if (m_impl) m_impl->deviceLost.store(true, std::memory_order_release);
}

GpuReadbackResult GpuRhiContext::importAndReadback(const GpuScopedNativeSurface& surface,
                                                   FramePixelFormat) noexcept {
#ifdef OLR_UNIT_TEST
    void* testHandle = surface.nativeHandle();
    m_lastReadbackHadNativeHandleForTest.store(testHandle != nullptr, std::memory_order_release);
    m_lastReadbackSubresourceForTest.store(surface.nativeSubresource(), std::memory_order_release);
    if (const auto injected = injectedReadbackForTest()) return *injected;
#endif
    void* handle = surface.nativeHandle();
    if (!surface.valid() || !handle) return {};
    try {
        if (!m_impl || !m_impl->valid) return {};
        if (m_impl->deviceLost.load(std::memory_order_acquire)) {
            GpuDeviceLossMonitor::instance().recordLoss();
            return {};
        }

        const uint64_t deviceAuthorityEpoch = m_impl->deviceAuthorityEpoch;
        const bool invoked = m_impl->thread.invoke([&] {
            QRhi* rhi = m_impl->thread.rhi;
            if (!rhi) return;
            const auto* nativeHandles =
                static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
            ID3D11Device* device =
                nativeHandles ? static_cast<ID3D11Device*>(nativeHandles->dev) : nullptr;
            const HRESULT removedReason = device ? device->GetDeviceRemovedReason() : HRESULT(S_OK);
            if (device && FAILED(removedReason)) {
                // LOCK RULE: D3D11 removed-device polling touches no m_bufferMutex.
                // Driver-authoritative loss: mint the provenance-bound token and hand
                // it to the loss latch so the worker's recovery can free held surfaces
                // WITHOUT waiting on the (now dead) fences.
                if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                        DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
                        deviceDomainId(device)) != 0)
                    m_impl->deviceLost.store(true, std::memory_order_release);
            }
        });
        (void) invoked;
        return {};
    } catch (...) {
        // This backend only polls device status; it never submits readback work.
        return {};
    }
}

std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    if (!m_impl || !m_impl->valid) return nullptr;

    std::shared_ptr<GpuFence> fence;
    const bool invoked = m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (!rhi) return;
        const auto* nativeHandles =
            static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        fence = nativeHandles ? makeD3D11GpuFence(nativeHandles->dev, m_impl->deviceAuthorityEpoch)
                              : nullptr;
    });
    return invoked ? fence : nullptr;
}

#ifdef OLR_UNIT_TEST
uint64_t GpuRhiContext::captureD3D11RemovalAuthorityForTest() {
    return GpuDeviceLossMonitor::instance().captureDeviceAuthorityEpoch();
}

D3D11RemovalObservationForTest
GpuRhiContext::observeD3D11RemovalForTest(void* opaqueDevice, uint64_t deviceAuthorityEpoch) {
    auto* device = static_cast<ID3D11Device*>(opaqueDevice);
    if (!device) return {};
    const HRESULT reason = device->GetDeviceRemovedReason();
    uint64_t generation = 0;
    if (FAILED(reason)) {
        generation = GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
            DeadDeviceToken::Provenance::DxgiDeviceRemovedReason, deviceAuthorityEpoch,
            deviceDomainId(device));
    }
    return D3D11RemovalObservationForTest{int64_t(reason), generation};
}
#endif

#endif // _WIN32
