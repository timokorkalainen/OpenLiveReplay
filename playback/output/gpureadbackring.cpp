#include "playback/output/gpureadbackring.h"

#include "playback/gpu/gpufence.h"
#include "playback/output/framehandle.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

namespace {

constexpr qint64 kDefaultReadyReadbackCacheBytes = 256ll * 1024ll * 1024ll;

qint64 planeBytes(const CpuPlanes& planes) {
    qint64 bytes = 0;
    for (const QByteArray& plane : planes.plane)
        bytes += plane.size();
    return bytes;
}

qint64 readyReadbackCacheBudgetBytes() {
    const QByteArray raw = qgetenv("OLR_GPU_READBACK_CACHE_MB").trimmed();
    bool ok = false;
    const qint64 mb = raw.toLongLong(&ok);
    if (ok) return qMax<qint64>(0, mb) * 1024ll * 1024ll;
    return kDefaultReadyReadbackCacheBytes;
}

GpuReadbackSurfaceKey surfaceKeyFor(const OutputBusFrame& frame, FramePixelFormat format) {
    const FrameMetadata& meta = frame.video.metadata();
    const quint32 videoHash =
        frame.identity.videoHash != 0 ? frame.identity.videoHash : meta.key.videoHash;
    return {frame.bus,
            -1,
            format,
            meta.gpuGeneration,
            meta.key.feedIndex,
            meta.key.ptsMs,
            meta.decodedSequence,
            videoHash,
            meta.key.width,
            meta.key.height,
            meta.key.isPlaceholder};
}

SharedGpuReadbackKey readbackKeyFor(const OutputBusFrame& frame, FramePixelFormat format) {
    return {surfaceKeyFor(frame, format), frame.video.metadata().gpuGeneration};
}

} // namespace

void SharedGpuReadbackCache::clear() {
    std::lock_guard<std::mutex> locker(m_mutex);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        Entry& entry = it.value();
        const bool keepEntry = entry.reading || entry.waiters > 0 || entry.retainedReaders > 0 ||
                               (entry.ready && entry.planes.isValid());
        if (keepEntry) {
            ++it;
        } else {
            eraseEntryLocked(it++);
        }
    }
    pruneReadyEntriesLocked();
    m_wake.notify_all();
}

void SharedGpuReadbackCache::retain(const OutputBusFrame& frame, FramePixelFormat format) {
    std::lock_guard<std::mutex> locker(m_mutex);
    Entry& entry = m_cache[readbackKeyFor(frame, format)];
    entry.retainedReaders++;
}

void SharedGpuReadbackCache::release(const OutputBusFrame& frame, FramePixelFormat format) {
    std::lock_guard<std::mutex> locker(m_mutex);
    auto it = m_cache.find(readbackKeyFor(frame, format));
    if (it == m_cache.end()) return;
    if (it.value().retainedReaders > 0) it.value().retainedReaders--;
    m_wake.notify_all();
}

bool SharedGpuReadbackCache::find(const OutputBusFrame& frame, FramePixelFormat format,
                                  CpuPlanes* planes) const {
    std::lock_guard<std::mutex> locker(m_mutex);
    const auto it = m_cache.constFind(readbackKeyFor(frame, format));
    if (it == m_cache.cend()) return false;
    if (!it.value().ready) return false;
    if (planes) *planes = it.value().planes;
    return true;
}

void SharedGpuReadbackCache::store(const OutputBusFrame& frame, FramePixelFormat format,
                                   const CpuPlanes& planes) {
    if (!planes.isValid()) return;
    std::lock_guard<std::mutex> locker(m_mutex);
    const SharedGpuReadbackKey key = readbackKeyFor(frame, format);
    Entry entry = m_cache.value(key);
    if (entry.ready && entry.planes.isValid()) m_readyBytes -= entry.bytes;
    entry.planes = planes;
    entry.ready = true;
    entry.reading = false;
    entry.bytes = planeBytes(planes);
    entry.lastUsed = ++m_useCounter;
    m_readyBytes += entry.bytes;
    m_cache.insert(key, entry);
    pruneReadyEntriesLocked();
    m_wake.notify_all();
}

CpuPlanes SharedGpuReadbackCache::getOrRead(const OutputBusFrame& frame, FramePixelFormat format,
                                            const std::function<CpuPlanes()>& reader,
                                            const std::function<bool()>& shouldCancel) {
    const SharedGpuReadbackKey key = readbackKeyFor(frame, format);
    {
        std::unique_lock<std::mutex> locker(m_mutex);
        while (true) {
            if (shouldCancel && shouldCancel()) return CpuPlanes{};
            auto it = m_cache.find(key);
            if (it == m_cache.end()) {
                Entry entry;
                entry.reading = true;
                entry.lastUsed = ++m_useCounter;
                m_cache.insert(key, entry);
                break;
            }
            if (it.value().ready) {
                it.value().lastUsed = ++m_useCounter;
                return it.value().planes;
            }
            if (!it.value().reading) {
                it.value().reading = true;
                it.value().lastUsed = ++m_useCounter;
                break;
            }
            it.value().waiters++;
            m_wake.wait_for(locker, std::chrono::milliseconds(10));
            auto afterWait = m_cache.find(key);
            if (afterWait != m_cache.end() && afterWait.value().waiters > 0)
                afterWait.value().waiters--;
        }
    }

    CpuPlanes planes = reader ? reader() : CpuPlanes{};
    {
        std::lock_guard<std::mutex> locker(m_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            if (it.value().ready && it.value().planes.isValid()) m_readyBytes -= it.value().bytes;
            if (planes.isValid()) {
                it.value().planes = planes;
                it.value().ready = true;
                it.value().reading = false;
                it.value().bytes = planeBytes(planes);
                it.value().lastUsed = ++m_useCounter;
                m_readyBytes += it.value().bytes;
            } else if (it.value().waiters > 0 || it.value().retainedReaders > 0) {
                it.value().planes = CpuPlanes{};
                it.value().ready = true;
                it.value().reading = false;
                it.value().bytes = 0;
                it.value().lastUsed = ++m_useCounter;
            } else {
                eraseEntryLocked(it);
            }
        }
        pruneReadyEntriesLocked();
        m_wake.notify_all();
    }
    return planes;
}

void SharedGpuReadbackCache::wakeAll() {
    m_wake.notify_all();
}

void SharedGpuReadbackCache::eraseEntryLocked(QHash<SharedGpuReadbackKey, Entry>::iterator it) {
    if (it == m_cache.end()) return;
    if (it.value().ready && it.value().planes.isValid()) m_readyBytes -= it.value().bytes;
    m_cache.erase(it);
}

void SharedGpuReadbackCache::pruneReadyEntriesLocked() {
    const qint64 budget = readyReadbackCacheBudgetBytes();
    if (budget <= 0) {
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            const Entry& entry = it.value();
            if (entry.ready && entry.planes.isValid() && !entry.reading && entry.waiters == 0 &&
                entry.retainedReaders == 0) {
                eraseEntryLocked(it++);
            } else {
                ++it;
            }
        }
        return;
    }

    while (m_readyBytes > budget) {
        auto oldest = m_cache.end();
        quint64 oldestUse = std::numeric_limits<quint64>::max();
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            const Entry& entry = it.value();
            if (!entry.ready || !entry.planes.isValid() || entry.reading || entry.waiters > 0 ||
                entry.retainedReaders > 0)
                continue;
            if (entry.lastUsed < oldestUse) {
                oldest = it;
                oldestUse = entry.lastUsed;
            }
        }
        if (oldest == m_cache.end()) break;
        eraseEntryLocked(oldest);
    }
}

GpuReadbackRing::GpuReadbackRing(int depth, std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks)
    : m_depth(std::max(1, depth)), m_sharedReadbacks(std::move(sharedReadbacks)) {}

GpuReadbackRing::~GpuReadbackRing() {
    releasePendingSharedReadbacks();
}

GpuReadbackRing::GpuReadbackRing(GpuReadbackRing&& other) noexcept
    : m_depth(other.m_depth), m_drops(other.m_drops),
      m_sharedReadbacks(std::move(other.m_sharedReadbacks)), m_pending(std::move(other.m_pending)) {
    other.m_drops = 0;
}

GpuReadbackRing& GpuReadbackRing::operator=(GpuReadbackRing&& other) noexcept {
    if (this == &other) return *this;
    releasePendingSharedReadbacks();
    m_depth = other.m_depth;
    m_drops = other.m_drops;
    m_sharedReadbacks = std::move(other.m_sharedReadbacks);
    m_pending = std::move(other.m_pending);
    other.m_drops = 0;
    return *this;
}

int GpuReadbackRing::occupancy() const {
    return static_cast<int>(m_pending.size());
}

RingReadyFrame GpuReadbackRing::pushAndPop(const OutputBusFrame& frame, uint64_t fenceValue,
                                           std::shared_ptr<GpuFence> fence,
                                           FramePixelFormat format) {
    const RingReadbackJob job = pushAndTakeReady(frame, fenceValue, std::move(fence), format);
    RingReadyFrame ready = readBack(job, m_sharedReadbacks);
    releaseSharedReadback(job);
    if (ready.readbackFailed) ++m_drops;
    return ready;
}

RingReadbackJob GpuReadbackRing::pushAndTakeReady(const OutputBusFrame& frame, uint64_t fenceValue,
                                                  std::shared_ptr<GpuFence> fence,
                                                  FramePixelFormat format) {
    const bool retainShared = m_sharedReadbacks != nullptr;
    if (retainShared) m_sharedReadbacks->retain(frame, format);
    m_pending.push_back(PendingFrame{frame, fenceValue, std::move(fence), format, retainShared});
    if (m_pending.size() < m_depth) return {};

    if (oldestReady()) return takeOldest();

    while (m_pending.size() > m_depth) {
        if (m_pending.front().retainedSharedReadback)
            m_sharedReadbacks->release(m_pending.front().frame, m_pending.front().format);
        m_pending.pop_front();
        ++m_drops;
    }
    return {};
}

RingReadyFrame GpuReadbackRing::flushOne(int timeoutMs) {
    const RingReadbackJob job = takeReadyAfterWait(timeoutMs);
    RingReadyFrame ready = readBack(job, m_sharedReadbacks);
    releaseSharedReadback(job);
    if (ready.readbackFailed) ++m_drops;
    return ready;
}

RingReadbackJob GpuReadbackRing::takeReadyAfterWait(int timeoutMs) {
    if (m_pending.isEmpty()) return {};

    const PendingFrame& oldest = m_pending.front();
    if (oldest.fence && !oldest.fence->wait(oldest.fenceValue, timeoutMs)) return {};

    return takeOldest();
}

RingReadyFrame GpuReadbackRing::readBackAndPopOldest() {
    RingReadyFrame ready = readBack(takeOldest(), m_sharedReadbacks);
    if (ready.readbackFailed) ++m_drops;
    return ready;
}

RingReadbackJob GpuReadbackRing::takeOldest() {
    if (m_pending.isEmpty()) return {};

    PendingFrame pending = std::move(m_pending.front());
    m_pending.pop_front();

    RingReadbackJob job;
    job.ready = true;
    job.frame = std::move(pending.frame);
    job.format = pending.format;
    return job;
}

void GpuReadbackRing::releasePendingSharedReadbacks() {
    if (!m_sharedReadbacks) {
        m_pending.clear();
        return;
    }
    for (const PendingFrame& pending : std::as_const(m_pending)) {
        if (pending.retainedSharedReadback)
            m_sharedReadbacks->release(pending.frame, pending.format);
    }
    m_pending.clear();
}

void GpuReadbackRing::releaseSharedReadback(const RingReadbackJob& job) const {
    if (!m_sharedReadbacks || !job.ready) return;
    m_sharedReadbacks->release(job.frame, job.format);
}

RingReadyFrame
GpuReadbackRing::readBack(const RingReadbackJob& job,
                          const std::shared_ptr<SharedGpuReadbackCache>& sharedReadbacks,
                          const std::function<bool()>& shouldCancel) {
    if (!job.ready) return {};

    CpuPlanes planes;
    if (sharedReadbacks) {
        const GpuReadbackSurfaceKey surfaceKey = surfaceKeyFor(job.frame, job.format);
        const bool gpuBacked = job.frame.video.isGpuBacked();
        planes = sharedReadbacks->getOrRead(
            job.frame, job.format,
            [&]() {
                CpuPlanes readback = job.frame.video.readToCpu(job.format);
                if (gpuBacked && readback.isValid()) {
                    GpuReadbackTelemetry::instance().recordSurface(surfaceKey);
                    GpuReadbackTelemetry::instance().recordGpuReadback(surfaceKey);
                }
                return readback;
            },
            shouldCancel);
    } else {
        const GpuReadbackSurfaceKey surfaceKey = surfaceKeyFor(job.frame, job.format);
        planes = job.frame.video.readToCpu(job.format);
        if (job.frame.video.isGpuBacked() && planes.isValid()) {
            GpuReadbackTelemetry::instance().recordSurface(surfaceKey);
            GpuReadbackTelemetry::instance().recordGpuReadback(surfaceKey);
        }
    }
    if (!planes.isValid()) {
        RingReadyFrame failed;
        failed.readbackFailed = true;
        return failed;
    }

    OutputBusFrame cpuFrame = job.frame;
    cpuFrame.video = makeCpuFrameHandle(std::move(planes), job.frame.video.metadata());
    RingReadyFrame ready;
    ready.ready = true;
    ready.frame = std::move(cpuFrame);
    return ready;
}

bool GpuReadbackRing::oldestReady() const {
    if (m_pending.isEmpty()) return false;

    const PendingFrame& oldest = m_pending.front();
    if (!oldest.fence || oldest.fenceValue == 0) return true;
    return oldest.fence->completedValue() >= oldest.fenceValue;
}
