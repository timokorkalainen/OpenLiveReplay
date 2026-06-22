#include "playback/gpu/gpurhicontext.h"

#ifdef __APPLE__

#include "playback/gpu/gpufence.h"

#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <QtGlobal>

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <Metal/Metal.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <cstring>
#include <functional>
#include <utility>

namespace {

qsizetype planeBytes(int stride, int rows) {
    return static_cast<qsizetype>(stride) * static_cast<qsizetype>(rows);
}

qsizetype byteOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

CpuPlanes lockDownloadNv12ToYuv420p(CVPixelBufferRef pb) {
    CpuPlanes out;
    if (!pb) return out;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return out;
    }

    const int w = static_cast<int>(CVPixelBufferGetWidth(pb));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pb));
    const int chromaW = (w + 1) / 2;
    const int chromaH = (h + 1) / 2;
    const auto* ySrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
    const auto* uvSrc = static_cast<const uchar*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
    if (!ySrc || !uvSrc) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return CpuPlanes{};
    }

    out.format = FramePixelFormat::Yuv420p;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = chromaW;
    out.stride[2] = chromaW;
    out.plane[0] = QByteArray(planeBytes(w, h), '\0');
    out.plane[1] = QByteArray(planeBytes(chromaW, chromaH), '\0');
    out.plane[2] = QByteArray(planeBytes(chromaW, chromaH), '\0');

    const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    for (int row = 0; row < h; ++row) {
        std::memcpy(out.plane[0].data() + byteOffset(row, out.stride[0]),
                    ySrc + static_cast<size_t>(row) * yStride, static_cast<size_t>(w));
    }

    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    char* u = out.plane[1].data();
    char* v = out.plane[2].data();
    for (int row = 0; row < chromaH; ++row) {
        const uchar* src = uvSrc + static_cast<size_t>(row) * uvStride;
        for (int x = 0; x < chromaW; ++x) {
            const qsizetype dst = byteOffset(row, chromaW) + x;
            const qsizetype srcIndex = static_cast<qsizetype>(2) * x;
            u[dst] = static_cast<char>(src[srcIndex]);
            v[dst] = static_cast<char>(src[srcIndex + 1]);
        }
    }

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return out;
}

class GpuRenderThread final : public QThread {
public:
    QRhi* rhi = nullptr;

    void run() override {
        QRhiMetalInitParams params;
        rhi = QRhi::create(QRhi::Metal, &params);
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
    GpuRenderThread thread;
    bool valid = false;
};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GpuRhiContext::~GpuRhiContext() {
    if (!m_impl) return;
    m_impl->thread.requestStop();
    m_impl->thread.wait();
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
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

CpuPlanes GpuRhiContext::importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                           FramePixelFormat target) {
    CpuPlanes result;
    if (target != FramePixelFormat::Yuv420p) {
        qWarning("GpuRhiContext::importAndReadback: Phase-2 readback is Yuv420p-only");
        return result;
    }
    if (!m_impl || !m_impl->valid || !surface || !surface->isValid()) return result;

    auto ioSurface = static_cast<IOSurfaceRef>(surface->nativeHandle());
    if (!ioSurface) return result;

    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, ioSurface, nullptr, &pb) !=
            kCVReturnSuccess ||
        !pb) {
        return result;
    }

    const bool invoked = m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (rhi) {
            QRhiCommandBuffer* cb = nullptr;
            if (rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess) {
                rhi->endOffscreenFrame();
            }
        }
        result = lockDownloadNv12ToYuv420p(pb);
    });
    CVPixelBufferRelease(pb);
    if (!invoked) return CpuPlanes{};
    return result;
}

std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    if (!m_impl || !m_impl->valid) return nullptr;

    std::shared_ptr<GpuFence> fence;
    const bool invoked = m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (!rhi) return;
        const auto* nativeHandles =
            static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
        fence = nativeHandles ? makeMetalGpuFence(nativeHandles->cmdQueue) : nullptr;
    });
    return invoked ? fence : nullptr;
}

#endif // __APPLE__
