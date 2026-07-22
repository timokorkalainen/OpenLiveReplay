#include "recorder_engine/codec/gpuencodepump.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#include <QString>

#include <limits>
#include <utility>

namespace {
constexpr int kFenceTimeoutMs = 100;

constexpr size_t kInvalidJobIndex = std::numeric_limits<size_t>::max();
} // namespace

GpuEncodePump::GpuEncodePump(NativeVideoEncoder* encoder, int maxQueue, std::mutex* encoderMutex)
    : m_encoder(encoder), m_encoderMutex(encoderMutex), m_maxQueue(maxQueue > 0 ? maxQueue : 1),
      m_slotCapacity(static_cast<size_t>(m_maxQueue) + 1),
      m_jobs(std::make_unique<Job[]>(m_slotCapacity)),
      m_queue(std::make_unique<size_t[]>(static_cast<size_t>(m_maxQueue))) {}

GpuEncodePump::~GpuEncodePump() {
    stop();
}

void GpuEncodePump::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    m_thread = std::thread([this] { run(); });
}

void GpuEncodePump::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running.store(false, std::memory_order_release);
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void GpuEncodePump::cancelPending() {
    for (;;) {
        JobCallbacks callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queueCount == 0) break;
            const size_t index = dequeueJobLocked();
            callbacks = m_jobs[index].callbacks;
            releaseJobSlotLocked(index);
            m_cv.notify_all();
        }
        // Failure may re-enter cancellation during a GPU-to-CPU fallback.
        failJob(callbacks);
    }
}

size_t GpuEncodePump::acquireJobSlotLocked() {
    for (size_t offset = 0; offset < m_slotCapacity; ++offset) {
        const size_t index = (m_nextJobSlot + offset) % m_slotCapacity;
        Job& slot = m_jobs[index];
        if (slot.active) continue;
        uint32_t generation = slot.generation + 1;
        if (generation == 0) generation = 1;
        slot = Job{};
        slot.active = true;
        slot.generation = generation;
        m_nextJobSlot = (index + 1) % m_slotCapacity;
        return index;
    }
    return kInvalidJobIndex;
}

void GpuEncodePump::releaseJobSlotLocked(size_t index) {
    Job& slot = m_jobs[index];
    const uint32_t generation = slot.generation;
    slot = Job{};
    slot.generation = generation;
}

void GpuEncodePump::enqueueJobLocked(size_t index) {
    m_queue[m_queueTail] = index;
    m_queueTail = (m_queueTail + 1) % static_cast<size_t>(m_maxQueue);
    ++m_queueCount;
}

size_t GpuEncodePump::dequeueJobLocked() {
    const size_t index = m_queue[m_queueHead];
    m_queueHead = (m_queueHead + 1) % static_cast<size_t>(m_maxQueue);
    --m_queueCount;
    return index;
}

bool GpuEncodePump::submit(FrameHandle frame, int64_t ptsTicks, ColorMetadata color,
                           JobCallbacks callbacks) {
    const IFrameData* frameData = frame.data();
    const GpuFrameSynchronization synchronization =
        frameData ? frameData->gpuSynchronization() : GpuFrameSynchronization{};
    if (!frameData || !frameData->isGpuBacked() || !synchronization.isExact()) {
        m_drops.fetch_add(1, std::memory_order_acq_rel);
        if (callbacks.onFailure) callbacks.onFailure(callbacks.context, callbacks.id);
        if (callbacks.onFinished) callbacks.onFinished(callbacks.context, callbacks.id);
        return false;
    }
    JobCallbacks rejectedCallbacks;
    bool rejected = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool runningAtEntry = m_running.load(std::memory_order_acquire);
        if (runningAtEntry) {
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) ||
                       m_queueCount < static_cast<size_t>(m_maxQueue);
            });
        }
        if ((runningAtEntry && !m_running.load(std::memory_order_acquire)) ||
            m_queueCount >= static_cast<size_t>(m_maxQueue)) {
            m_drops.fetch_add(1, std::memory_order_acq_rel);
            rejectedCallbacks = callbacks;
            rejected = true;
        } else {
            const size_t index = acquireJobSlotLocked();
            if (index == kInvalidJobIndex) {
                m_drops.fetch_add(1, std::memory_order_acq_rel);
                rejectedCallbacks = callbacks;
                rejected = true;
            } else {
                Job& job = m_jobs[index];
                job.frame = std::move(frame);
                job.synchronization = synchronization;
                job.ptsTicks = ptsTicks;
                job.color = color;
                job.callbacks = callbacks;
                enqueueJobLocked(index);
            }
        }
    }
    if (rejected) {
        if (rejectedCallbacks.onFailure)
            rejectedCallbacks.onFailure(rejectedCallbacks.context, rejectedCallbacks.id);
        if (rejectedCallbacks.onFinished)
            rejectedCallbacks.onFinished(rejectedCallbacks.context, rejectedCallbacks.id);
        return false;
    }
    m_cv.notify_one();
    return true;
}

void GpuEncodePump::run() {
    for (;;) {
        size_t jobIndex = kInvalidJobIndex;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) || m_queueCount != 0;
            });
            if (!m_running.load(std::memory_order_acquire) && m_queueCount == 0) return;
            jobIndex = dequeueJobLocked();
            m_cv.notify_all();
        }
        Job& job = m_jobs[jobIndex];

        // FENCE-BEFORE-ENCODE: never read a surface the producer is still writing.
        if (job.synchronization.value != 0 &&
            !job.synchronization.fence->wait(job.synchronization.value, kFenceTimeoutMs)) {
            failJob(job.callbacks);
            std::lock_guard<std::mutex> lock(m_mutex);
            releaseJobSlotLocked(jobIndex);
            continue;
        }

        const IFrameData* frameData = job.frame.data();
        GpuSurface* surface = frameData ? frameData->gpuSurface() : nullptr;
        if (!surface || !m_encoder) {
            failJob(job.callbacks);
            std::lock_guard<std::mutex> lock(m_mutex);
            releaseJobSlotLocked(jobIndex);
            continue;
        }

        QString error;
        bool encoded = false;
        {
            std::unique_lock<std::mutex> encoderLock;
            if (m_encoderMutex) encoderLock = std::unique_lock<std::mutex>(*m_encoderMutex);
            auto collectPacket = [&](const QByteArray& data, int64_t ptsTicks, bool keyframe) {
                if (job.packetCount >= job.packets.size()) {
                    job.packetOverflow = true;
                    return;
                }
                EncodedPacket& packet = job.packets[job.packetCount++];
                packet.data = data;
                packet.ptsTicks = ptsTicks;
                packet.keyframe = keyframe;
            };
            encoded = m_encoder->encodeSurface(
                surface, job.ptsTicks, job.color,
                NativeVideoEncoder::PacketCallback::bind(collectPacket), &error);
        }

        if (encoded && !job.packetOverflow) {
            if (job.callbacks.onPacket) {
                for (size_t i = 0; i < job.packetCount; ++i) {
                    const EncodedPacket& packet = job.packets[i];
                    job.callbacks.onPacket(job.callbacks.context, job.callbacks.id, packet.data,
                                           packet.ptsTicks, packet.keyframe);
                }
            }
            m_encoded.fetch_add(1, std::memory_order_acq_rel);
            if (job.callbacks.onFinished)
                job.callbacks.onFinished(job.callbacks.context, job.callbacks.id);
        } else {
            failJob(job.callbacks);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        releaseJobSlotLocked(jobIndex);
        m_cv.notify_all();
    }
}

void GpuEncodePump::failJob(JobCallbacks callbacks) {
    m_drops.fetch_add(1, std::memory_order_acq_rel);
    if (callbacks.onFailure) callbacks.onFailure(callbacks.context, callbacks.id);
    if (callbacks.onFinished) callbacks.onFinished(callbacks.context, callbacks.id);
}

uint64_t GpuEncodePump::queueDrops() const {
    return m_drops.load(std::memory_order_acquire);
}

uint64_t GpuEncodePump::framesEncoded() const {
    return m_encoded.load(std::memory_order_acquire);
}
