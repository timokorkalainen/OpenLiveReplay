#ifndef OLR_GPU_FRAME_RETIRE_QUEUE_H
#define OLR_GPU_FRAME_RETIRE_QUEUE_H

#include "playback/output/framehandle.h"

#include <QVector>
#include <cstdint>
#include <memory>
#include <utility>

class GpuFence;

class GpuFrameRetireQueue {
public:
    void collect(FrameHandle frame);
    void collect(const QVector<FrameHandle>& frames);
    int drain(int timeoutMs, int* stalls = nullptr, int maxWaits = -1);
    void append(GpuFrameRetireQueue&& other);
    void swap(GpuFrameRetireQueue& other) noexcept;

    // Allocation-free partition used by terminal teardown after exact
    // device-loss cleanup has established which individual owners are safe.
    template <typename RetiredPredicate>
    int releaseRetiredFrames(RetiredPredicate&& retired) noexcept {
        int released = 0;
        auto survivor = m_entries.begin();
        for (auto current = m_entries.begin(); current != m_entries.end(); ++current) {
            if (retired(current->frame)) {
                released++;
                continue;
            }
            if (survivor != current) *survivor = std::move(*current);
            ++survivor;
        }
        m_entries.erase(survivor, m_entries.end());
        return released;
    }

    // Allocation-free inspection used only by bounded teardown recovery.
    template <typename Visitor>
    void forEachFrame(Visitor&& visitor) const {
        for (const auto& entry : m_entries)
            visitor(entry.frame);
    }

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
