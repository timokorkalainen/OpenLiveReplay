#ifndef OLR_GPU_FRAME_RETIRE_QUEUE_H
#define OLR_GPU_FRAME_RETIRE_QUEUE_H

#include "playback/output/framehandle.h"

#include <QVector>
#include <cstdint>
#include <memory>

class GpuFence;

class GpuFrameRetireQueue {
public:
    void collect(FrameHandle frame, std::shared_ptr<GpuFence> fence);
    void collect(const QVector<FrameHandle>& frames, std::shared_ptr<GpuFence> fence);
    int drain(int timeoutMs, int* stalls = nullptr);
    void append(GpuFrameRetireQueue&& other);
    void swap(GpuFrameRetireQueue& other);

    int size() const { return static_cast<int>(m_entries.size()); }
    bool isEmpty() const { return m_entries.isEmpty(); }

private:
    struct Entry {
        FrameHandle frame;
        std::shared_ptr<GpuFence> fence;
        uint64_t fenceValue = 0;
    };

    QVector<Entry> m_entries;
};

#endif // OLR_GPU_FRAME_RETIRE_QUEUE_H
