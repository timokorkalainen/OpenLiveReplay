#include "playback/gpu/gpuframeretirequeue.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <utility>

void GpuFrameRetireQueue::collect(FrameHandle frame, std::shared_ptr<GpuFence> fence) {
    if (!fence) return;
    if (!frame.isGpuBacked() || !frame.data()) return;

    GpuSurface* surface = frame.data()->gpuSurface();
    if (!surface) return;

    const uint64_t fenceValue = surface->pendingFenceValue();
    if (fenceValue == 0) return;

    m_entries.append(Entry{std::move(frame), std::move(fence), fenceValue});
}

void GpuFrameRetireQueue::collect(const QVector<FrameHandle>& frames,
                                  std::shared_ptr<GpuFence> fence) {
    for (const FrameHandle& frame : frames)
        collect(frame, fence);
}

int GpuFrameRetireQueue::drain(int timeoutMs, int* stalls, int maxWaits) {
    if (m_entries.isEmpty()) return 0;

    QVector<Entry> pending;
    int released = 0;
    int waits = 0;
    for (Entry& entry : m_entries) {
        bool retired = false;
        if (entry.fence) {
            retired = entry.fence->completedValue() >= entry.fenceValue;
            if (!retired && (maxWaits < 0 || waits < maxWaits)) {
                waits++;
                retired = entry.fence->wait(entry.fenceValue, timeoutMs);
            }
        }

        if (retired) {
            released++;
        } else {
            if (stalls) (*stalls)++;
            pending.append(std::move(entry));
        }
    }

    m_entries = std::move(pending);
    return released;
}

void GpuFrameRetireQueue::append(GpuFrameRetireQueue&& other) {
    for (Entry& entry : other.m_entries)
        m_entries.append(std::move(entry));
    other.m_entries.clear();
}

void GpuFrameRetireQueue::swap(GpuFrameRetireQueue& other) noexcept {
    m_entries.swap(other.m_entries);
}
