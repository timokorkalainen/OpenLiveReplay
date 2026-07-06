#include "playback/output/gpureadbackring.h"

#include "playback/gpu/gpufence.h"
#include "playback/output/framehandle.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace {

GpuReadbackSurfaceKey surfaceKeyFor(const OutputBusFrame& frame, FramePixelFormat format) {
    const FrameMetadata& meta = frame.video.metadata();
    return {frame.bus,      frame.outputFrameIndex, format, meta.gpuGeneration, meta.key.feedIndex,
            meta.key.ptsMs, meta.decodedSequence};
}

SharedGpuReadbackKey readbackKeyFor(const OutputBusFrame& frame, FramePixelFormat format) {
    return {surfaceKeyFor(frame, format), frame.video.metadata().gpuGeneration};
}

} // namespace

void SharedGpuReadbackCache::clear() {
    std::lock_guard<std::mutex> locker(m_mutex);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        Entry& entry = it.value();
        if (entry.reading || entry.waiters > 0 || entry.retainedReaders > 0) {
            ++it;
        } else {
            it = m_cache.erase(it);
        }
    }
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
    entry.planes = planes;
    entry.ready = true;
    entry.reading = false;
    m_cache.insert(key, entry);
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
                m_cache.insert(key, Entry{CpuPlanes{}, false, true, 0, 0});
                break;
            }
            if (it.value().ready) return it.value().planes;
            if (!it.value().reading) {
                it.value().reading = true;
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
            if (planes.isValid()) {
                it.value().planes = planes;
                it.value().ready = true;
                it.value().reading = false;
            } else if (it.value().waiters > 0 || it.value().retainedReaders > 0) {
                it.value().planes = CpuPlanes{};
                it.value().ready = true;
                it.value().reading = false;
            } else {
                m_cache.erase(it);
            }
        }
        m_wake.notify_all();
    }
    return planes;
}

void SharedGpuReadbackCache::wakeAll() {
    m_wake.notify_all();
}

GpuReadbackRing::GpuReadbackRing(int depth, std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks)
    : m_depth(std::max(1, depth)), m_sharedReadbacks(std::move(sharedReadbacks)) {}

int GpuReadbackRing::occupancy() const {
    return static_cast<int>(m_pending.size());
}

RingReadyFrame GpuReadbackRing::pushAndPop(const OutputBusFrame& frame, uint64_t fenceValue,
                                           std::shared_ptr<GpuFence> fence,
                                           FramePixelFormat format) {
    RingReadyFrame ready =
        readBack(pushAndTakeReady(frame, fenceValue, std::move(fence), format), m_sharedReadbacks);
    if (ready.readbackFailed) ++m_drops;
    return ready;
}

RingReadbackJob GpuReadbackRing::pushAndTakeReady(const OutputBusFrame& frame, uint64_t fenceValue,
                                                  std::shared_ptr<GpuFence> fence,
                                                  FramePixelFormat format) {
    m_pending.push_back(PendingFrame{frame, fenceValue, std::move(fence), format});
    if (m_pending.size() < m_depth) return {};

    if (oldestReady()) return takeOldest();

    while (m_pending.size() > m_depth) {
        m_pending.pop_front();
        ++m_drops;
    }
    return {};
}

RingReadyFrame GpuReadbackRing::flushOne(int timeoutMs) {
    if (m_pending.isEmpty()) return {};

    const PendingFrame& oldest = m_pending.front();
    if (oldest.fence && !oldest.fence->wait(oldest.fenceValue, timeoutMs)) return {};

    return readBackAndPopOldest();
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
