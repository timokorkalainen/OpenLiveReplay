#ifndef OLR_GPUENCODEPUMP_H
#define OLR_GPUENCODEPUMP_H

#include "playback/output/colormetadata.h"
#include "playback/output/framehandle.h"

#include <QByteArray>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class GpuFence;
class NativeVideoEncoder;

class GpuEncodePump {
public:
    using PacketSink = std::function<void(const QByteArray& data, int64_t ptsTicks, bool keyframe)>;
    using FailureSink = std::function<void()>;

    GpuEncodePump(NativeVideoEncoder* encoder, int maxQueue = 4,
                  std::mutex* encoderMutex = nullptr);
    ~GpuEncodePump();

    GpuEncodePump(const GpuEncodePump&) = delete;
    GpuEncodePump& operator=(const GpuEncodePump&) = delete;

    bool submit(FrameHandle frame, int64_t ptsTicks, ColorMetadata color, PacketSink onPacket,
                FailureSink onFailure = FailureSink{});

    void start();
    void stop();
    void cancelPending();

    uint64_t queueDrops() const;
    uint64_t framesEncoded() const;

private:
    struct Job {
        FrameHandle frame;
        GpuFrameSynchronization synchronization;
        int64_t ptsTicks = 0;
        ColorMetadata color;
        PacketSink onPacket;
        FailureSink onFailure;
    };

    void failJob(Job& job);

    void run();

    NativeVideoEncoder* m_encoder = nullptr;
    std::mutex* m_encoderMutex = nullptr;
    int m_maxQueue = 4;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_drops{0};
    std::atomic<uint64_t> m_encoded{0};
};

#endif // OLR_GPUENCODEPUMP_H
