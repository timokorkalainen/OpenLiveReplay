#include "playback/frameindex.h"

#include <algorithm>

void FrameIndex::append(qint64 ptsMs, qint64 byteOffset) {
    // Keep ptsMs strictly increasing; ignore out-of-order or duplicate PTS.
    if (!m_entries.empty() && ptsMs <= m_entries.back().ptsMs) {
        return;
    }
    m_entries.push_back(Entry{ptsMs, byteOffset});
}

std::optional<qint64> FrameIndex::nearestAtOrBefore(qint64 ptsMs) const {
    if (m_entries.empty()) {
        return std::nullopt;
    }
    // First entry with ptsMs > target; the one before it is the answer.
    auto it = std::upper_bound(m_entries.begin(), m_entries.end(), ptsMs,
                               [](qint64 value, const Entry& e) { return value < e.ptsMs; });
    if (it == m_entries.begin()) {
        return std::nullopt; // target is before the first indexed frame
    }
    --it;
    return it->byteOffset;
}

std::optional<qint64> FrameIndex::newestPtsMs() const {
    if (m_entries.empty()) {
        return std::nullopt;
    }
    return m_entries.back().ptsMs;
}
