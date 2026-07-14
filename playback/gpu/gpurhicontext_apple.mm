#include "playback/gpu/gpurhicontext.h"

#ifdef __APPLE__

#include "playback/gpu/appleiosurface.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpusurfacelease.h"
#include "playback/output/formatcanon.h"

#include <QList>
#include <QThread>
#include <QtGlobal>

#include <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <Metal/Metal.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

// The ONLY definition of the Apple/RHI mint (friend of DeadDeviceToken). Global
// scope so it matches the friend + namespace-scope declaration in gpusurfacelease.h.
// Reached only from the FrameOpDeviceLost / isDeviceLost() branches below, so no
// other TU can construct a DeadDeviceToken from the Apple backend.
namespace {

uintptr_t metalDeviceDomainId(QRhi* rhi) {
    const auto* handles =
        rhi ? static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles()) : nullptr;
    id<MTLCommandQueue> queue = handles ? static_cast<id<MTLCommandQueue>>(handles->cmdQueue) : nil;
    return reinterpret_cast<uintptr_t>(queue ? queue.device : nil);
}

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

CpuPlanes lockDownloadRgba8(CVPixelBufferRef pb) {
    CpuPlanes out;
    if (!pb) return out;
    const OSType pixelFormat = CVPixelBufferGetPixelFormatType(pb);
    if (pixelFormat != kCVPixelFormatType_32RGBA && pixelFormat != kCVPixelFormatType_32BGRA) {
        return out;
    }
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return out;
    }

    const int w = static_cast<int>(CVPixelBufferGetWidth(pb));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pb));
    const auto* src = static_cast<const uchar*>(CVPixelBufferGetBaseAddress(pb));
    const size_t srcStride = CVPixelBufferGetBytesPerRow(pb);
    if (!src || srcStride < static_cast<size_t>(w) * 4) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return CpuPlanes{};
    }

    out.format = FramePixelFormat::Rgba8;
    out.width = w;
    out.height = h;
    out.stride[0] = w * 4;
    out.plane[0] = QByteArray(planeBytes(out.stride[0], h), '\0');
    for (int row = 0; row < h; ++row) {
        const uchar* srcRow = src + static_cast<size_t>(row) * srcStride;
        char* dstRow = out.plane[0].data() + byteOffset(row, out.stride[0]);
        if (pixelFormat == kCVPixelFormatType_32RGBA) {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                const qsizetype dst = static_cast<qsizetype>(x) * 4;
                const qsizetype srcOffset = static_cast<qsizetype>(x) * 4;
                dstRow[dst] = char(srcRow[srcOffset + 2]);
                dstRow[dst + 1] = char(srcRow[srcOffset + 1]);
                dstRow[dst + 2] = char(srcRow[srcOffset]);
                dstRow[dst + 3] = char(srcRow[srcOffset + 3]);
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    return out;
}

CVMetalTextureCacheRef makeTextureCache(QRhi* rhi) {
    if (!rhi) return nullptr;
    const auto* nativeHandles = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
    if (!nativeHandles || !nativeHandles->dev) return nullptr;
    id<MTLDevice> device = (__bridge id<MTLDevice>)nativeHandles->dev;

    CVMetalTextureCacheRef cache = nullptr;
    const CVReturn rc =
        CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &cache);
    return rc == kCVReturnSuccess ? cache : nullptr;
}

CFDictionaryRef makeMetalTextureAttributes(MTLTextureUsage usage) {
    uint64_t usageValue = static_cast<uint64_t>(usage);
    CFNumberRef usageNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &usageValue);
    if (!usageNumber) return nullptr;

    const void* keys[] = {kCVMetalTextureUsage};
    const void* values[] = {usageNumber};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(usageNumber);
    return attrs;
}

CpuPlanes rgba8FromReadback(const QRhiReadbackResult& readback, const GpuSurfaceDesc& desc) {
    CpuPlanes out;
    if (desc.width <= 0 || desc.height <= 0) return out;
    if (readback.data.size() < desc.width * desc.height * 4) return out;
    if (readback.format != QRhiTexture::RGBA8 && readback.format != QRhiTexture::BGRA8) {
        return out;
    }

    out.format = FramePixelFormat::Rgba8;
    out.width = desc.width;
    out.height = desc.height;
    out.stride[0] = desc.width * 4;
    out.plane[0] = QByteArray(planeBytes(out.stride[0], desc.height), '\0');

    const auto* src = reinterpret_cast<const uchar*>(readback.data.constData());
    auto* dst = reinterpret_cast<uchar*>(out.plane[0].data());
    const qsizetype bytes = static_cast<qsizetype>(desc.width) * desc.height * 4;
    if (readback.format == QRhiTexture::RGBA8) {
        std::memcpy(dst, src, static_cast<size_t>(bytes));
    } else {
        for (qsizetype i = 0; i < bytes; i += 4) {
            dst[i] = src[i + 2];
            dst[i + 1] = src[i + 1];
            dst[i + 2] = src[i];
            dst[i + 3] = src[i + 3];
        }
    }
    return out;
}

int readbackRowStride(const QRhiReadbackResult& readback, int minBytesPerRow, int rows) {
    if (minBytesPerRow <= 0 || rows <= 0) return 0;
    const qsizetype minimumBytes =
        static_cast<qsizetype>(minBytesPerRow) * static_cast<qsizetype>(rows);
    if (readback.data.size() < minimumBytes) return 0;
    if (readback.data.size() % rows == 0) {
        const qsizetype inferred = readback.data.size() / rows;
        if (inferred >= minBytesPerRow && inferred <= std::numeric_limits<int>::max()) {
            return static_cast<int>(inferred);
        }
    }
    return minBytesPerRow;
}

CpuPlanes yuv420pFromNv12Readbacks(const QRhiReadbackResult& yReadback,
                                   const QRhiReadbackResult& uvReadback,
                                   const GpuSurfaceDesc& desc) {
    CpuPlanes out;
    if (desc.width <= 0 || desc.height <= 0 || desc.format != FramePixelFormat::Nv12) {
        return out;
    }
    const int chromaW = (desc.width + 1) / 2;
    const int chromaH = (desc.height + 1) / 2;
    if (yReadback.format != QRhiTexture::R8 || uvReadback.format != QRhiTexture::RG8 ||
        yReadback.pixelSize != QSize(desc.width, desc.height) ||
        uvReadback.pixelSize != QSize(chromaW, chromaH)) {
        return out;
    }

    const int yReadStride = readbackRowStride(yReadback, desc.width, desc.height);
    const int uvReadStride = readbackRowStride(uvReadback, chromaW * 2, chromaH);
    if (yReadStride <= 0 || uvReadStride <= 0) return out;

    out.format = FramePixelFormat::Yuv420p;
    out.width = desc.width;
    out.height = desc.height;
    out.stride[0] = desc.width;
    out.stride[1] = chromaW;
    out.stride[2] = chromaW;
    out.plane[0] = QByteArray(planeBytes(out.stride[0], desc.height), '\0');
    out.plane[1] = QByteArray(planeBytes(out.stride[1], chromaH), '\0');
    out.plane[2] = QByteArray(planeBytes(out.stride[2], chromaH), '\0');

    const auto* ySrc = reinterpret_cast<const uchar*>(yReadback.data.constData());
    const auto* uvSrc = reinterpret_cast<const uchar*>(uvReadback.data.constData());
    for (int row = 0; row < desc.height; ++row) {
        std::memcpy(out.plane[0].data() + byteOffset(row, out.stride[0]),
                    ySrc + byteOffset(row, yReadStride), static_cast<size_t>(desc.width));
    }
    for (int row = 0; row < chromaH; ++row) {
        const uchar* src = uvSrc + byteOffset(row, uvReadStride);
        char* u = out.plane[1].data() + byteOffset(row, out.stride[1]);
        char* v = out.plane[2].data() + byteOffset(row, out.stride[2]);
        for (int x = 0; x < chromaW; ++x) {
            const int srcOffset = x * 2;
            u[x] = static_cast<char>(src[srcOffset]);
            v[x] = static_cast<char>(src[srcOffset + 1]);
        }
    }
    return out;
}

CpuPlanes readbackRgba8WithRhi(QRhi* rhi, CVPixelBufferRef pb, const GpuSurfaceDesc& desc) {
    CpuPlanes out;
    if (!rhi || !pb || desc.format != FramePixelFormat::Rgba8 || desc.width <= 0 ||
        desc.height <= 0) {
        return out;
    }

    CVMetalTextureCacheRef cache = makeTextureCache(rhi);
    if (!cache) return out;

    CFDictionaryRef readAttrs = makeMetalTextureAttributes(MTLTextureUsageShaderRead);
    if (!readAttrs) {
        CFRelease(cache);
        return out;
    }

    CVMetalTextureRef cvTexture = nullptr;
    const CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, pb, readAttrs, MTLPixelFormatBGRA8Unorm, desc.width,
        desc.height, 0, &cvTexture);
    CFRelease(readAttrs);
    if (rc != kCVReturnSuccess || !cvTexture) {
        CFRelease(cache);
        return out;
    }

    id<MTLTexture> metalTexture = CVMetalTextureGetTexture(cvTexture);
    if (!metalTexture) {
        CFRelease(cvTexture);
        CFRelease(cache);
        return out;
    }

    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(
        QRhiTexture::BGRA8, QSize(desc.width, desc.height), 1, QRhiTexture::UsedAsTransferSource));
    if (!texture) {
        CFRelease(cvTexture);
        CFRelease(cache);
        return out;
    }

    const QRhiTexture::NativeTexture nativeTexture{
        quint64(reinterpret_cast<uintptr_t>((__bridge void*)metalTexture)), 0};
    if (!texture->createFrom(nativeTexture)) {
        CFRelease(cvTexture);
        CFRelease(cache);
        return out;
    }

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        CFRelease(cvTexture);
        CFRelease(cache);
        return out;
    }

    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    if (batch) {
        batch->readBackTexture(QRhiReadbackDescription(texture.get()), &readback);
        cb->resourceUpdate(batch);
    }
    const QRhi::FrameOpResult end = rhi->endOffscreenFrame();
    if (end == QRhi::FrameOpSuccess && rhi->finish() == QRhi::FrameOpSuccess) {
        out = rgba8FromReadback(readback, desc);
    }

    CFRelease(cvTexture);
    CFRelease(cache);
    return out;
}

CpuPlanes readbackNv12WithRhi(QRhi* rhi, CVPixelBufferRef pb, const GpuSurfaceDesc& desc) {
    CpuPlanes out;
    if (!rhi || !pb || desc.format != FramePixelFormat::Nv12 || desc.width <= 0 ||
        desc.height <= 0) {
        return out;
    }

    CVMetalTextureCacheRef cache = makeTextureCache(rhi);
    if (!cache) return out;

    CFDictionaryRef readAttrs = makeMetalTextureAttributes(MTLTextureUsageShaderRead);
    if (!readAttrs) {
        CFRelease(cache);
        return out;
    }

    CVMetalTextureRef luma = nullptr;
    CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb,
                                                            readAttrs, MTLPixelFormatR8Unorm,
                                                            desc.width, desc.height, 0, &luma);
    if (rc != kCVReturnSuccess || !luma) {
        CFRelease(readAttrs);
        CFRelease(cache);
        return out;
    }

    const int chromaW = (desc.width + 1) / 2;
    const int chromaH = (desc.height + 1) / 2;
    CVMetalTextureRef chroma = nullptr;
    rc = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, readAttrs,
                                                   MTLPixelFormatRG8Unorm, chromaW, chromaH, 1,
                                                   &chroma);
    CFRelease(readAttrs);
    if (rc != kCVReturnSuccess || !chroma) {
        CFRelease(luma);
        CFRelease(cache);
        return out;
    }

    id<MTLTexture> lumaTexture = CVMetalTextureGetTexture(luma);
    id<MTLTexture> chromaTexture = CVMetalTextureGetTexture(chroma);
    if (!lumaTexture || !chromaTexture) {
        CFRelease(chroma);
        CFRelease(luma);
        CFRelease(cache);
        return out;
    }

    std::unique_ptr<QRhiTexture> yTex(rhi->newTexture(
        QRhiTexture::R8, QSize(desc.width, desc.height), 1, QRhiTexture::UsedAsTransferSource));
    std::unique_ptr<QRhiTexture> uvTex(rhi->newTexture(QRhiTexture::RG8, QSize(chromaW, chromaH), 1,
                                                       QRhiTexture::UsedAsTransferSource));
    if (!yTex || !uvTex) {
        CFRelease(chroma);
        CFRelease(luma);
        CFRelease(cache);
        return out;
    }

    const QRhiTexture::NativeTexture yNative{
        quint64(reinterpret_cast<uintptr_t>((__bridge void*)lumaTexture)), 0};
    const QRhiTexture::NativeTexture uvNative{
        quint64(reinterpret_cast<uintptr_t>((__bridge void*)chromaTexture)), 0};
    if (!yTex->createFrom(yNative) || !uvTex->createFrom(uvNative)) {
        CFRelease(chroma);
        CFRelease(luma);
        CFRelease(cache);
        return out;
    }

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb) {
        CFRelease(chroma);
        CFRelease(luma);
        CFRelease(cache);
        return out;
    }

    QRhiReadbackResult yReadback;
    QRhiReadbackResult uvReadback;
    QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
    if (batch) {
        batch->readBackTexture(QRhiReadbackDescription(yTex.get()), &yReadback);
        batch->readBackTexture(QRhiReadbackDescription(uvTex.get()), &uvReadback);
        cb->resourceUpdate(batch);
    }

    const QRhi::FrameOpResult end = rhi->endOffscreenFrame();
    if (end == QRhi::FrameOpSuccess && rhi->finish() == QRhi::FrameOpSuccess) {
        out = yuv420pFromNv12Readbacks(yReadback, uvReadback, desc);
    }

    CFRelease(chroma);
    CFRelease(luma);
    CFRelease(cache);
    return out;
}

// RENDER-THREAD INVARIANT (iOS main-thread rule): this thread runs only QRhi/Metal
// command encoding. It must not call UIKit or touch a CAMetalLayer; use
// GpuRhiContext::presentOnMainThread for any present/UIKit interaction.
class GpuRenderThread final : public QThread {
public:
    explicit GpuRenderThread(QRhi::Implementation backend) : m_backend(backend) {}

    QRhi* rhi = nullptr;

    void run() override {
        QRhiMetalInitParams metalParams;
        QRhiNullInitParams nullParams;
        QRhiInitParams* params = nullptr;
        switch (m_backend) {
        case QRhi::Metal:
            params = &metalParams;
            break;
        case QRhi::Null:
            params = &nullParams;
            break;
        default:
            break;
        }
        QRhi* createdRhi = params ? QRhi::create(m_backend, params) : nullptr;
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
    QRhi::Implementation m_backend = QRhi::Null;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    QList<std::function<void()>> m_jobs;
    bool m_ready = false;
    bool m_stop = false;
};

} // namespace

class GpuRhiContext::Impl {
public:
    explicit Impl(QRhi::Implementation backend) : thread(backend), backend(backend) {}

    GpuRenderThread thread;
    QRhi::Implementation backend = QRhi::Null;
    bool valid = false;
    uint64_t deviceAuthorityEpoch = 0;
    std::atomic<bool> deviceLost{false};
#ifdef OLR_UNIT_TEST
    std::atomic<int> rhiReadbacks{0};
#endif
};

GpuRhiContext::GpuRhiContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GpuRhiContext::~GpuRhiContext() {
    if (!m_impl) return;
    m_impl->thread.requestStop();
    m_impl->thread.wait();
}

std::shared_ptr<GpuRhiContext> GpuRhiContext::create() {
    auto impl = std::make_unique<Impl>(QRhi::Metal);
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
    auto impl = std::make_unique<Impl>(QRhi::Null);
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

std::shared_ptr<GpuRhiContext> GpuRhiContext::createWarpForTest() {
    return nullptr;
}

#ifdef OLR_UNIT_TEST
std::shared_ptr<GpuRhiContext> GpuRhiContext::createInvalidForTest() {
    return std::shared_ptr<GpuRhiContext>(new GpuRhiContext(std::make_unique<Impl>(QRhi::Null)));
}

int GpuRhiContext::rhiReadbackCountForTest() const {
    return m_impl ? m_impl->rhiReadbacks.load(std::memory_order_acquire) : 0;
}
#endif

bool GpuRhiContext::isValid() const {
    return m_impl && m_impl->valid;
}

bool GpuRhiContext::isNullBackend() const {
    return m_impl && m_impl->backend == QRhi::Null;
}

bool GpuRhiContext::invokeOnRenderThread(const std::function<void(QRhi*)>& job) const {
    if (!m_impl || !m_impl->valid || !job) return false;
    return m_impl->thread.invoke([&] { job(m_impl->thread.rhi); });
}

void GpuRhiContext::presentOnMainThread(const std::function<void()>& block) {
    if (!block) return;
#if TARGET_OS_IOS
    // MAIN-THREAD: iOS requires UIKit/CAMetalLayer present work on the main thread.
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{
          block();
        });
    }
#else
    block();
#endif
}

bool GpuRhiContext::deviceLost() const {
    return m_impl && m_impl->deviceLost.load(std::memory_order_acquire);
}

bool GpuRhiContext::pollDeviceLoss() const {
    if (!m_impl || !m_impl->valid) return false;
    if (m_impl->deviceLost.load(std::memory_order_acquire)) return true;
    const uint64_t authority = m_impl->deviceAuthorityEpoch;
    m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (!rhi || !rhi->isDeviceLost()) return;
        if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                DeadDeviceToken::Provenance::RhiFrameOpDeviceLost, authority,
                metalDeviceDomainId(rhi)) != 0)
            m_impl->deviceLost.store(true, std::memory_order_release);
    });
    return m_impl->deviceLost.load(std::memory_order_acquire);
}

void GpuRhiContext::injectDeviceLostForTest() {
    if (m_impl) m_impl->deviceLost.store(true, std::memory_order_release);
}

CpuPlanes GpuRhiContext::importAndReadback(const std::shared_ptr<GpuSurface>& surface,
                                           FramePixelFormat target) {
    CpuPlanes result;
    if (!m_impl || !m_impl->valid) {
        return result;
    }
    if (m_impl->deviceLost.load(std::memory_order_acquire)) {
        GpuDeviceLossMonitor::instance().recordLoss();
        return result;
    }
    if (!surface || !surface->isValid()) {
        return result;
    }
    const GpuSurfaceDesc desc = surface->desc();

    CVPixelBufferRef pb = retainApplePixelBufferWrapper(surface);
    if (!pb) return result;

    const uint64_t deviceAuthorityEpoch = m_impl->deviceAuthorityEpoch;
    const bool invoked = m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (rhi) {
            QRhiCommandBuffer* cb = nullptr;
            const QRhi::FrameOpResult begin = rhi->beginOffscreenFrame(&cb);
            if (begin == QRhi::FrameOpSuccess) {
                const QRhi::FrameOpResult end = rhi->endOffscreenFrame();
                if (end == QRhi::FrameOpDeviceLost || rhi->isDeviceLost()) {
                    // LOCK RULE: this render-thread poll touches no m_bufferMutex;
                    // callers observe deviceLost() and degrade/rebuild outside it.
                    // Driver-authoritative loss: mint the provenance-bound token so
                    // the worker's recovery frees held surfaces without waiting on
                    // the dead device's fences.
                    if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                            DeadDeviceToken::Provenance::RhiFrameOpDeviceLost, deviceAuthorityEpoch,
                            metalDeviceDomainId(rhi)) != 0)
                        m_impl->deviceLost.store(true, std::memory_order_release);
                    result = CpuPlanes{};
                    return;
                }
            } else {
                if (begin == QRhi::FrameOpDeviceLost || rhi->isDeviceLost()) {
                    // LOCK RULE: this render-thread poll touches no m_bufferMutex.
                    if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                            DeadDeviceToken::Provenance::RhiFrameOpDeviceLost, deviceAuthorityEpoch,
                            metalDeviceDomainId(rhi)) != 0)
                        m_impl->deviceLost.store(true, std::memory_order_release);
                }
                result = CpuPlanes{};
                return;
            }
        }
        if (desc.format == FramePixelFormat::Rgba8 && target == FramePixelFormat::Rgba8) {
            result = readbackRgba8WithRhi(rhi, pb, desc);
            if (result.isValid()) {
#ifdef OLR_UNIT_TEST
                m_impl->rhiReadbacks.fetch_add(1, std::memory_order_acq_rel);
#endif
                return;
            }
            if (rhi && rhi->isDeviceLost()) {
                if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                        DeadDeviceToken::Provenance::RhiFrameOpDeviceLost, deviceAuthorityEpoch,
                        metalDeviceDomainId(rhi)) != 0)
                    m_impl->deviceLost.store(true, std::memory_order_release);
                result = CpuPlanes{};
                return;
            }
        }
        if (desc.format == FramePixelFormat::Nv12) {
            if (target == FramePixelFormat::Yuv420p || target == FramePixelFormat::Nv12) {
                CpuPlanes yuv = readbackNv12WithRhi(rhi, pb, desc);
                if (yuv.isValid()) {
#ifdef OLR_UNIT_TEST
                    m_impl->rhiReadbacks.fetch_add(1, std::memory_order_acq_rel);
#endif
                    result =
                        target == FramePixelFormat::Yuv420p ? yuv : formatcanon::yuv420pToNv12(yuv);
                    if (result.isValid()) return;
                }
                if (rhi && rhi->isDeviceLost()) {
                    if (GpuDeviceLossMonitor::instance().publishRealDeviceLoss(
                            DeadDeviceToken::Provenance::RhiFrameOpDeviceLost, deviceAuthorityEpoch,
                            metalDeviceDomainId(rhi)) != 0)
                        m_impl->deviceLost.store(true, std::memory_order_release);
                    result = CpuPlanes{};
                    return;
                }
                if (target == FramePixelFormat::Yuv420p) {
                    result = lockDownloadNv12ToYuv420p(pb);
                } else {
                    result = formatcanon::yuv420pToNv12(lockDownloadNv12ToYuv420p(pb));
                }
            }
        } else if (desc.format == FramePixelFormat::Rgba8 && target == FramePixelFormat::Rgba8) {
            result = lockDownloadRgba8(pb);
        }
    });
    CVPixelBufferRelease(pb);
    if (!invoked) return CpuPlanes{};
    return result;
}

std::shared_ptr<GpuFence> GpuRhiContext::createFence() const {
    if (!m_impl || !m_impl->valid) return nullptr;
    if (m_impl->backend == QRhi::Null) return GpuFence::create();

    std::shared_ptr<GpuFence> fence;
    const bool invoked = m_impl->thread.invoke([&] {
        QRhi* rhi = m_impl->thread.rhi;
        if (!rhi) return;
        const auto* nativeHandles =
            static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
        fence = nativeHandles
                    ? makeMetalGpuFence(nativeHandles->cmdQueue, m_impl->deviceAuthorityEpoch)
                    : nullptr;
    });
    return invoked ? fence : nullptr;
}

#endif // __APPLE__
