#ifndef OLR_GPUENCODEPUMP_H
#define OLR_GPUENCODEPUMP_H

#include "playback/output/colormetadata.h"
#include "playback/output/framehandle.h"

#include <QByteArray>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

class GpuFence;
class NativeVideoEncoder;

class GpuEncodePump {
public:
    struct JobCallbacks {
        using PacketFunction = void (*)(void* context, uint64_t id, const QByteArray& data,
                                        int64_t ptsTicks, bool keyframe);
        using StateFunction = void (*)(void* context, uint64_t id);

        void* context = nullptr;
        uint64_t id = 0;
        PacketFunction onPacket = nullptr;
        StateFunction onFailure = nullptr;
        StateFunction onFinished = nullptr;
    };
    static_assert(std::is_trivially_copyable_v<JobCallbacks>);
    static constexpr int kMaxPacketsPerJob = 8;

    GpuEncodePump(NativeVideoEncoder* encoder, std::shared_ptr<GpuFence> fence, int maxQueue = 4,
                  std::mutex* encoderMutex = nullptr);
    ~GpuEncodePump();

    GpuEncodePump(const GpuEncodePump&) = delete;
    GpuEncodePump& operator=(const GpuEncodePump&) = delete;

    bool submit(FrameHandle frame, uint64_t fenceValue, int64_t ptsTicks, ColorMetadata color,
                JobCallbacks callbacks);

    void start();
    void stop();
    void cancelPending();

    uint64_t queueDrops() const;
    uint64_t framesEncoded() const;

private:
    struct EncodedPacket {
        QByteArray data;
        int64_t ptsTicks = 0;
        bool keyframe = false;
    };
    struct Job {
        bool active = false;
        uint32_t generation = 0;
        FrameHandle frame;
        uint64_t fenceValue = 0;
        int64_t ptsTicks = 0;
        ColorMetadata color;
        JobCallbacks callbacks;
        std::array<EncodedPacket, kMaxPacketsPerJob> packets;
        size_t packetCount = 0;
        bool packetOverflow = false;
    };

    void failJob(JobCallbacks callbacks);
    size_t acquireJobSlotLocked();
    void releaseJobSlotLocked(size_t index);
    void enqueueJobLocked(size_t index);
    size_t dequeueJobLocked();

    void run();

    NativeVideoEncoder* m_encoder = nullptr;
    std::mutex* m_encoderMutex = nullptr;
    std::shared_ptr<GpuFence> m_fence;
    int m_maxQueue = 4;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    size_t m_slotCapacity = 0;
    std::unique_ptr<Job[]> m_jobs;
    std::unique_ptr<size_t[]> m_queue;
    size_t m_queueHead = 0;
    size_t m_queueTail = 0;
    size_t m_queueCount = 0;
    size_t m_nextJobSlot = 0;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_drops{0};
    std::atomic<uint64_t> m_encoded{0};
};

#endif // OLR_GPUENCODEPUMP_H
