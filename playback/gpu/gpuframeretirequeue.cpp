#include "playback/gpu/gpuframeretirequeue.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"

#include <type_traits>
#include <utility>

void GpuFrameRetireQueue::collect(FrameHandle frame) {
    if (!frame.isGpuBacked() || !frame.data()) return;
    GpuFrameSynchronization synchronization = frame.data()->gpuSynchronization();
    if (!synchronization.isExact() || synchronization.value == 0) return;
    m_entries.append(
        Entry{std::move(frame), std::move(synchronization.fence), synchronization.value});
}

void GpuFrameRetireQueue::collect(const QVector<FrameHandle>& frames) {
    for (const FrameHandle& frame : frames)
        collect(frame);
}

int GpuFrameRetireQueue::drain(int timeoutMs, int* stalls, int maxWaits) {
    if (m_entries.isEmpty()) return 0;

    static_assert(std::is_nothrow_move_assignable_v<Entry>);
    int released = 0;
    int waits = 0;
    auto survivor = m_entries.begin();
    for (auto current = m_entries.begin(); current != m_entries.end(); ++current) {
        bool retired = false;
        try {
            if (current->fence) {
                retired = current->fence->completedValue() >= current->fenceValue;
                if (!retired && (maxWaits < 0 || waits < maxWaits)) {
                    waits++;
                    retired = current->fence->wait(current->fenceValue, timeoutMs);
                }
            }
        } catch (...) {
            retired = false;
        }

        if (retired) {
            released++;
        } else {
            if (survivor != current) *survivor = std::move(*current);
            ++survivor;
            if (stalls) (*stalls)++;
        }
    }

    m_entries.erase(survivor, m_entries.end());
    return released;
}

void GpuFrameRetireQueue::append(GpuFrameRetireQueue&& other) {
    if (other.m_entries.isEmpty()) return;
    static_assert(std::is_nothrow_move_constructible_v<Entry>);
    if (m_entries.isEmpty()) {
        m_entries.swap(other.m_entries);
        return;
    }

    m_entries.reserve(m_entries.size() + other.m_entries.size());
    for (Entry& entry : other.m_entries)
        m_entries.append(std::move(entry));
    other.m_entries.clear();
}

void GpuFrameRetireQueue::swap(GpuFrameRetireQueue& other) noexcept {
    m_entries.swap(other.m_entries);
}
