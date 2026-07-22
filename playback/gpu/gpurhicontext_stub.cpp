#include "playback/gpu/gpurhicontext.h"

#ifndef __APPLE__

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"

#include <QList>
#include <QThread>
#include <rhi/qrhi.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <utility>

namespace {

class NullRenderThread final : public QThread {
public:
    QRhi* rhi = nullptr;

    void run() override {
        QRhiNullInitParams params;
        QRhi* createdRhi = QRhi::create(QRhi::Null, &params);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            rhi = createdRhi;
            m_ready = true;
        }
        m_cond.notify_all();

        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cond.wait(lock, [&] { return !m_jobs.isEmpty() || m_stop; });
                if (m_stop && m_jobs.isEmpty()) break;
                job = m_jobs.takeFirst();
            }
            job();
        }

        delete rhi;
        rhi = nullptr;
    }

    bool waitReady() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond.wait(lock, [&] { return m_ready; });
        return rhi != nullptr;
    }

    bool invoke(std::function<void()> job) {
        if (QThread::currentThread() == this) {
            try {
                job();
                return true;
            } catch (...) {
                return false;
            }
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stop) return false;

        bool done = false;
        bool succeeded = false;
        m_jobs.append([&] {
            try {
                job();
                succeeded = true;
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> doneLock(m_mutex);
                done = true;
            }
            m_cond.notify_all();
        });
        m_cond.notify_all();
        m_cond.wait(lock, [&] { return done; });
        return succeeded;
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cond.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    QList<std::function<void()>> m_jobs;
    bool m_ready = false;
    bool m_stop = false;
};

} // namespace

class GpuRhiContext::Impl {
public:
    NullRenderThread thread;
    bool valid = false;
    std::atomic<bool> deviceLost{false};
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
    m_impl->thread.requestStop();
    m_impl->thread.wait();
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
    return nullptr;
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::createNullForTest() {
    auto impl = std::make_unique<Impl>();
    impl->thread.start();
    impl->valid = impl->thread.waitReady();
    if (!impl->valid) {
        impl->thread.requestStop();
        impl->thread.wait();
        return nullptr;
    }
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::move(impl)));
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::createWarpForTest() {
    return nullptr;
}

#ifdef OLR_UNIT_TEST
std::shared_ptr<GpuRhiContext> GpuRhiContext::createInvalidForTest() {
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::make_unique<Impl>()));
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::createReadbackFenceFailureForTest() {
    auto impl = std::make_unique<Impl>();
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
#endif

bool GpuRhiContext::isValid() const {
    return m_impl && m_impl->valid;
}

bool GpuRhiContext::isNullBackend() const {
    return isValid();
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
    return deviceLost();
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
        }
        return {};
    } catch (...) {
        // This backend never submits readback work.
        return {};
    }
}

std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    return isValid() ? GpuFence::create() : nullptr;
}

#endif // !__APPLE__
