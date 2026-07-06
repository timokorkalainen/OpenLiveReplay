#include "recorder_engine/codec/gpuencodepump.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "recorder_engine/codec/nativevideoencoder.h"

#include <QString>

#include <utility>
#include <vector>

namespace {
constexpr int kFenceTimeoutMs = 100;

struct EncodedPacket {
    QByteArray data;
    int64_t ptsTicks = 0;
    bool keyframe = false;
};
} // namespace

GpuEncodePump::GpuEncodePump(NativeVideoEncoder* encoder, std::shared_ptr<GpuFence> fence,
                             int maxQueue, std::mutex* encoderMutex)
    : m_encoder(encoder), m_encoderMutex(encoderMutex), m_fence(std::move(fence)),
      m_maxQueue(maxQueue > 0 ? maxQueue : 1) {}

GpuEncodePump::~GpuEncodePump() {
    stop();
}

void GpuEncodePump::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) return;
    m_thread = std::thread([this] { run(); });
}

void GpuEncodePump::stop() {
    m_running.store(false, std::memory_order_release);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void GpuEncodePump::cancelPending() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_drops.fetch_add(m_queue.size(), std::memory_order_acq_rel);
    m_queue.clear();
}

bool GpuEncodePump::submit(FrameHandle frame, uint64_t fenceValue, int64_t ptsTicks,
                           ColorMetadata color, PacketSink onPacket, FailureSink onFailure) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (int(m_queue.size()) >= m_maxQueue) {
            m_queue.pop_front();
            m_drops.fetch_add(1, std::memory_order_acq_rel);
        }

        m_queue.push_back(Job{std::move(frame), fenceValue, ptsTicks, color, std::move(onPacket),
                              std::move(onFailure)});
    }
    m_cv.notify_one();
    return true;
}

void GpuEncodePump::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (!m_running.load(std::memory_order_acquire) && m_queue.empty()) return;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }

        const IFrameData* frameData = job.frame.data();
        std::shared_ptr<GpuFence> fence = frameData ? frameData->gpuFence() : nullptr;
        if (!fence) fence = m_fence;

        // FENCE-BEFORE-ENCODE: never read a surface the producer is still writing.
        if (fence && !fence->wait(job.fenceValue, kFenceTimeoutMs)) {
            failJob(job);
            continue;
        }

        GpuSurface* surface = frameData ? frameData->gpuSurface() : nullptr;
        if (!surface || !m_encoder) {
            failJob(job);
            continue;
        }

        QString error;
        std::vector<EncodedPacket> packets;
        bool encoded = false;
        {
            std::unique_lock<std::mutex> encoderLock;
            if (m_encoderMutex) encoderLock = std::unique_lock<std::mutex>(*m_encoderMutex);
            encoded = m_encoder->encodeSurface(
                surface, job.ptsTicks, job.color,
                [&packets](const QByteArray& data, int64_t ptsTicks, bool keyframe) {
                    packets.push_back(EncodedPacket{data, ptsTicks, keyframe});
                },
                &error);
        }

        if (encoded) {
            if (job.onPacket) {
                for (const EncodedPacket& packet : packets)
                    job.onPacket(packet.data, packet.ptsTicks, packet.keyframe);
            }
            m_encoded.fetch_add(1, std::memory_order_acq_rel);
        } else {
            failJob(job);
        }
    }
}

void GpuEncodePump::failJob(Job& job) {
    m_drops.fetch_add(1, std::memory_order_acq_rel);
    if (job.onFailure) job.onFailure();
}

uint64_t GpuEncodePump::queueDrops() const {
    return m_drops.load(std::memory_order_acquire);
}

uint64_t GpuEncodePump::framesEncoded() const {
    return m_encoded.load(std::memory_order_acquire);
}
