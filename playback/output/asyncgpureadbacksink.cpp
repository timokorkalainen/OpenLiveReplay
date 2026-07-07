#include "playback/output/asyncgpureadbacksink.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpusurface.h"

#include <algorithm>
#include <mutex>
#include <utility>

AsyncGpuReadbackSink::AsyncGpuReadbackSink(std::unique_ptr<IOutputSink> inner, int ringDepth,
                                           FramePixelFormat cpuFormat, SinkGpuCapability capability,
                                           std::shared_ptr<GpuFence> renderFence,
                                           std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks,
                                           bool innerAlreadyStarted)
    : m_inner(std::move(inner)), m_innerAlreadyStarted(innerAlreadyStarted),
      m_kind(m_inner ? m_inner->kind() : OutputTargetKind::QtPreview),
      m_ringDepth(std::max(1, ringDepth)), m_ring(m_ringDepth, sharedReadbacks),
      m_cpuFormat(cpuFormat), m_capability(capability),
      m_renderFence(renderFence ? std::move(renderFence) : GpuFence::create()),
      m_sharedReadbacks(std::move(sharedReadbacks)) {}

AsyncGpuReadbackSink::~AsyncGpuReadbackSink() {
    stop();
}

bool AsyncGpuReadbackSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    if (m_active.load(std::memory_order_acquire) || m_thread) stop();
    if (!m_inner) return false;
    const bool readbackEnabled = gpuPipelineEnabled();
    {
        std::lock_guard<std::mutex> innerLocker(m_innerMutex);
        if (m_innerAlreadyStarted) {
            if (!m_inner->isActive()) return false;
        } else if (!m_inner->start(assignment, rate)) {
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> locker(m_mutex);
        m_ring = GpuReadbackRing(m_ringDepth, m_sharedReadbacks);
        m_jobs.clear();
        m_hasLastDelivered = false;
        m_hasLastIdentity = false;
        m_hasGpuGeneration = false;
        m_lastGpuGeneration = 0;
        m_generationDrops = 0;
        m_asyncReadbackDrops = 0;
        m_readbackInFlight = false;
        m_stopRequested.store(false, std::memory_order_release);
        m_active.store(true, std::memory_order_release);
        m_readbackEnabled.store(readbackEnabled, std::memory_order_release);
        m_needsReadbackCadence.store(false, std::memory_order_release);
        m_cancelReadbacks.store(false, std::memory_order_release);
        ++m_epoch;
    }

    if (readbackEnabled) {
        m_thread.reset(QThread::create([this]() { workerLoop(); }));
        m_thread->start();
    }
    return true;
}

void AsyncGpuReadbackSink::stop() {
    std::unique_ptr<QThread> thread;
    {
        std::lock_guard<std::mutex> locker(m_mutex);
        m_active.store(false, std::memory_order_release);
        m_stopRequested.store(true, std::memory_order_release);
        m_readbackEnabled.store(false, std::memory_order_release);
        m_needsReadbackCadence.store(false, std::memory_order_release);
        m_cancelReadbacks.store(true, std::memory_order_release);
        clearPendingReadbacksLocked();
        m_wake.notify_all();
        if (m_sharedReadbacks) m_sharedReadbacks->wakeAll();
        thread = std::move(m_thread);
    }

    if (thread) thread->wait();
    if (m_inner) {
        std::lock_guard<std::mutex> innerLocker(m_innerMutex);
        m_inner->stop();
    }
}

bool AsyncGpuReadbackSink::isActive() const {
    std::lock_guard<std::mutex> innerLocker(m_innerMutex);
    return m_active.load(std::memory_order_acquire) &&
           !m_stopRequested.load(std::memory_order_acquire) && m_inner && m_inner->isActive();
}

bool AsyncGpuReadbackSink::needsContinuousCadence() const {
    if (m_capability == SinkGpuCapability::NeedsContinuousCadence) return true;
    if (m_inner && m_inner->needsContinuousCadence()) return true;
    return m_readbackEnabled.load(std::memory_order_acquire) &&
           m_needsReadbackCadence.load(std::memory_order_acquire);
}

bool AsyncGpuReadbackSink::submit(const OutputBusFrame& frame) {
    if (!m_inner) return false;

    const bool gpuRuntimeEnabled =
        m_readbackEnabled.load(std::memory_order_acquire) && gpuPipelineEnabled();

    if (!gpuRuntimeEnabled || !frame.video.isGpuBacked()) {
        {
            std::lock_guard<std::mutex> locker(m_mutex);
            if (!m_active.load(std::memory_order_acquire) ||
                m_stopRequested.load(std::memory_order_acquire))
                return false;
            if (gpuRuntimeEnabled && !frame.video.isGpuBacked() && m_hasGpuGeneration) {
                m_generationDrops += m_ring.drops() + m_ring.occupancy() + m_jobs.size() +
                                     (m_readbackInFlight ? 1 : 0);
                clearPendingReadbacksLocked();
                m_hasLastDelivered = false;
                m_hasLastIdentity = false;
                m_hasGpuGeneration = false;
                m_lastGpuGeneration = 0;
                m_needsReadbackCadence.store(false, std::memory_order_release);
            }
        }
        bool ok = false;
        {
            std::lock_guard<std::mutex> innerLocker(m_innerMutex);
            ok = m_inner && m_active.load(std::memory_order_acquire) &&
                 !m_stopRequested.load(std::memory_order_acquire) && m_inner->submit(frame);
        }
        if (ok) rememberDelivered(frame);
        return ok;
    }

    uint64_t fenceValue = 0;
    std::shared_ptr<GpuFence> producerFence = m_renderFence;
    bool hasExplicitProducerFence = false;
    if (const IFrameData* data = frame.video.data()) {
        if (GpuSurface* surface = data->gpuSurface()) fenceValue = surface->pendingFenceValue();
        if (std::shared_ptr<GpuFence> frameFence = data->gpuFence()) {
            producerFence = std::move(frameFence);
            hasExplicitProducerFence = true;
        }
    }

    OutputBusFrame cadenceFrame;
    bool submitCadenceFrame = false;
    {
        std::lock_guard<std::mutex> locker(m_mutex);
        if (!m_active.load(std::memory_order_acquire) ||
            m_stopRequested.load(std::memory_order_acquire))
            return false;

        const uint64_t gpuGeneration = frame.video.metadata().gpuGeneration;
        if (!m_hasLastIdentity || !m_lastIdentity.samePayloadAs(frame.identity))
            m_needsReadbackCadence.store(true, std::memory_order_release);
        if (gpuGeneration != 0) {
            if (m_hasGpuGeneration && m_lastGpuGeneration != gpuGeneration) {
                m_generationDrops += m_ring.drops() + m_ring.occupancy() + m_jobs.size() +
                                     (m_readbackInFlight ? 1 : 0);
                clearPendingReadbacksLocked();
                m_hasLastDelivered = false;
                m_hasLastIdentity = false;
                m_needsReadbackCadence.store(true, std::memory_order_release);
            }
            m_lastGpuGeneration = gpuGeneration;
            m_hasGpuGeneration = true;
        }

        RingReadbackJob ready;
        if (fenceValue == 0 && !hasExplicitProducerFence) {
            ++m_asyncReadbackDrops;
            if (m_capability == SinkGpuCapability::NeedsContinuousCadence && m_hasLastDelivered) {
                cadenceFrame = m_lastDelivered;
                submitCadenceFrame = true;
            } else {
                return true;
            }
        } else {
            if (fenceValue == 0 && producerFence) fenceValue = producerFence->completedValue();
            ready =
                m_ring.pushAndTakeReady(frame, fenceValue, std::move(producerFence), m_cpuFormat);
        }
        if (ready.ready) {
            if (m_sharedReadbacks) m_sharedReadbacks->retain(ready.frame, ready.format);
            m_jobs.append(QueuedReadbackJob{std::move(ready), m_epoch});
            m_wake.notify_one();
            return true;
        }

        if (!submitCadenceFrame && m_capability == SinkGpuCapability::NeedsContinuousCadence &&
            m_hasLastDelivered) {
            cadenceFrame = m_lastDelivered;
            submitCadenceFrame = true;
        }
    }

    bool cadenceOk = true;
    if (submitCadenceFrame) {
        std::lock_guard<std::mutex> innerLocker(m_innerMutex);
        cadenceOk = m_inner && m_active.load(std::memory_order_acquire) &&
                    !m_stopRequested.load(std::memory_order_acquire) &&
                    m_inner->submit(cadenceFrame);
    }

    return cadenceOk;
}

OutputSinkStatus AsyncGpuReadbackSink::outputStatus() const {
    OutputSinkStatus status;
    {
        std::lock_guard<std::mutex> innerLocker(m_innerMutex);
        status = m_inner ? m_inner->outputStatus() : OutputSinkStatus{};
    }
    qint64 drops = 0;
    qint64 depth = 0;
    readbackStats(depth, drops);
    status.droppedFrames += drops;
    status.currentQueueDepth += depth;
    status.maxQueueDepth = qMax(status.maxQueueDepth, depth);
    return status;
}

bool AsyncGpuReadbackSink::readbackStats(qint64& depth, qint64& drops) const {
    if (!m_readbackEnabled.load(std::memory_order_acquire)) {
        depth = 0;
        drops = 0;
        return false;
    }
    std::lock_guard<std::mutex> locker(m_mutex);
    depth = m_ring.occupancy() + m_jobs.size() + (m_readbackInFlight ? 1 : 0);
    drops = m_ring.drops() + m_generationDrops + m_asyncReadbackDrops;
    return true;
}

qint64 AsyncGpuReadbackSink::readbackDrops() const {
    if (!m_readbackEnabled.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> locker(m_mutex);
    return m_ring.drops() + m_generationDrops + m_asyncReadbackDrops;
}

qint64 AsyncGpuReadbackSink::readbackQueueDepth() const {
    if (!m_readbackEnabled.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> locker(m_mutex);
    return m_ring.occupancy() + m_jobs.size() + (m_readbackInFlight ? 1 : 0);
}

void AsyncGpuReadbackSink::workerLoop() {
    while (true) {
        QueuedReadbackJob queued;
        {
            std::unique_lock<std::mutex> locker(m_mutex);
            m_wake.wait(locker, [this]() {
                return m_stopRequested.load(std::memory_order_acquire) || !m_jobs.isEmpty();
            });
            if (m_stopRequested.load(std::memory_order_acquire)) return;
            queued = std::move(m_jobs.front());
            m_jobs.removeFirst();
            m_readbackInFlight = true;
        }

        RingReadyFrame ready = GpuReadbackRing::readBack(queued.job, m_sharedReadbacks, [this]() {
            return m_cancelReadbacks.load(std::memory_order_acquire);
        });
        if (m_sharedReadbacks) m_sharedReadbacks->release(queued.job.frame, queued.job.format);

        OutputBusFrame failedCadenceFrame;
        bool submitFailedCadenceFrame = false;
        {
            std::lock_guard<std::mutex> locker(m_mutex);
            if (m_stopRequested.load(std::memory_order_acquire) || queued.epoch != m_epoch) {
                m_readbackInFlight = false;
                m_wake.notify_all();
                continue;
            }
            if (ready.readbackFailed) {
                m_readbackInFlight = false;
                ++m_asyncReadbackDrops;
                if (m_capability == SinkGpuCapability::NeedsContinuousCadence &&
                    m_hasLastDelivered) {
                    failedCadenceFrame = m_lastDelivered;
                    submitFailedCadenceFrame = true;
                }
                m_wake.notify_all();
                if (!submitFailedCadenceFrame) continue;
            } else if (!ready.ready) {
                m_readbackInFlight = false;
                m_wake.notify_all();
                continue;
            }
            if (m_capability == SinkGpuCapability::AsyncReadbackDedupOk && m_hasLastIdentity &&
                m_lastIdentity.samePayloadAs(ready.frame.identity)) {
                m_readbackInFlight = false;
                m_lastIdentity = ready.frame.identity;
                m_hasLastIdentity = true;
                m_wake.notify_all();
                continue;
            }
        }
        if (submitFailedCadenceFrame) {
            bool ok = false;
            {
                std::lock_guard<std::mutex> innerLocker(m_innerMutex);
                ok = m_inner && m_active.load(std::memory_order_acquire) &&
                     !m_stopRequested.load(std::memory_order_acquire) &&
                     m_inner->submit(failedCadenceFrame);
            }
            if (ok) rememberDelivered(failedCadenceFrame);
            continue;
        }

        bool ok = false;
        {
            std::lock_guard<std::mutex> innerLocker(m_innerMutex);
            ok = m_inner && m_active.load(std::memory_order_acquire) &&
                 !m_stopRequested.load(std::memory_order_acquire) && m_inner->submit(ready.frame);
        }
        {
            std::lock_guard<std::mutex> locker(m_mutex);
            m_readbackInFlight = false;
            if (ok && !m_stopRequested.load(std::memory_order_acquire) && queued.epoch == m_epoch)
                rememberDeliveredLocked(ready.frame);
            m_wake.notify_all();
        }
    }
}

void AsyncGpuReadbackSink::rememberDelivered(const OutputBusFrame& frame) {
    std::lock_guard<std::mutex> locker(m_mutex);
    rememberDeliveredLocked(frame);
}

void AsyncGpuReadbackSink::rememberDeliveredLocked(const OutputBusFrame& frame) {
    m_lastDelivered = frame;
    m_hasLastDelivered = true;
    m_lastIdentity = frame.identity;
    m_hasLastIdentity = true;
    m_needsReadbackCadence.store(false, std::memory_order_release);
}

void AsyncGpuReadbackSink::clearPendingReadbacksLocked() {
    if (m_sharedReadbacks) {
        for (const QueuedReadbackJob& queued : std::as_const(m_jobs)) {
            m_sharedReadbacks->release(queued.job.frame, queued.job.format);
        }
    }
    m_jobs.clear();
    m_ring = GpuReadbackRing(m_ringDepth, m_sharedReadbacks);
    ++m_epoch;
}
