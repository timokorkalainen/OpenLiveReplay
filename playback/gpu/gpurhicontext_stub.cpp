#include "playback/gpu/gpurhicontext.h"

#ifndef __APPLE__

#include "playback/gpu/gpufence.h"

#include <QList>
#include <QThread>
#include <rhi/qrhi.h>

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
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_stop) return false;

        bool done = false;
        m_jobs.append([&] {
            job();
            {
                std::lock_guard<std::mutex> doneLock(m_mutex);
                done = true;
            }
            m_cond.notify_all();
        });
        m_cond.notify_all();
        m_cond.wait(lock, [&] { return done; });
        return true;
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
};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

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

bool GpuRhiContext::isValid() const {
    return m_impl && m_impl->valid;
}

bool GpuRhiContext::isNullBackend() const {
    return isValid();
}

bool GpuRhiContext::invokeOnRenderThread(const std::function<void(QRhi*)>& job) const {
    if (!m_impl || !m_impl->valid || !job) return false;
    return m_impl->thread.invoke([&] { job(m_impl->thread.rhi); });
}

CpuPlanes GpuRhiContext::importAndReadback(const std::shared_ptr<GpuSurface>&, FramePixelFormat) {
    return CpuPlanes{};
}

std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    return GpuFence::create();
}

#endif // !__APPLE__
