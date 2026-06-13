#include "playback/audioframequeue.h"

void AudioFrameQueue::enqueue(int64_t ptsMs, const char* data, int bytes) {
    m_q.enqueue(Frame{ptsMs, QByteArray(data, bytes)});
}

bool AudioFrameQueue::releaseDue(int64_t playheadMs, int64_t leadMs, Frame& out) {
    if (m_q.isEmpty()) return false;
    if (m_q.head().ptsMs <= playheadMs + leadMs) {
        out = m_q.dequeue();
        return true;
    }
    return false;
}

void AudioFrameQueue::dropOlderThan(int64_t playheadMs, int64_t keepBehindMs) {
    const int64_t threshold = playheadMs - keepBehindMs;
    while (!m_q.isEmpty() && m_q.head().ptsMs < threshold)
        m_q.dequeue();
}

void AudioFrameQueue::clear() {
    m_q.clear();
}

int AudioFrameQueue::spanMs() const {
    if (m_q.size() < 2) return 0;
    return static_cast<int>(m_q.last().ptsMs - m_q.head().ptsMs);
}
