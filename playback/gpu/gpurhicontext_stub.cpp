#include "playback/gpu/gpurhicontext.h"

#ifndef __APPLE__

#include "playback/gpu/gpufence.h"

#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <rhi/qrhi.h>

#include <functional>
#include <utility>

namespace {

class NullRenderThread final : public QThread {
public:
    QRhi* rhi = nullptr;

    void run() override {
        QRhiNullInitParams params;
        rhi = QRhi::create(QRhi::Null, &params);
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
    QMutex m_mutex;
    QWaitCondition m_cond;
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
