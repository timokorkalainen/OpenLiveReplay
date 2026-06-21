#include "playback/trackbuffer.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// insert
// ---------------------------------------------------------------------------
bool TrackBuffer::insert(int64_t ptsMs, const FrameHandle& f, int capFrames, int64_t keepNearMs,
                         int64_t protectToMs) {
    // Binary-search for the insertion position (lower_bound by ptsMs).
    int lo = 0, hi = m_frames.size();
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (m_frames[mid].ptsMs < ptsMs)
            lo = mid + 1;
        else
            hi = mid;
    }
    // lo is the first index where m_frames[lo].ptsMs >= ptsMs.

    if (lo < m_frames.size() && m_frames[lo].ptsMs == ptsMs) {
        // Duplicate PTS: replace frame, size unchanged.
        m_frames[lo].frame = f;
        return true;
    }

    // Insert new entry at lo.
    m_frames.insert(lo, Frame{ptsMs, f});

    // Enforce the cap.
    if (capFrames > 0 && m_frames.size() > capFrames) {
        // Find the entry to evict: farthest from keepNearMs among those
        // whose PTS is outside [keepNearMs, protectToMs].
        int evictIdx = -1;
        int64_t maxDist = -1;

        for (int i = 0; i < m_frames.size(); ++i) {
            int64_t pts = m_frames[i].ptsMs;
            if (pts >= keepNearMs && pts <= protectToMs) continue; // protected range — skip
            int64_t dist = pts >= keepNearMs ? (pts - keepNearMs) : (keepNearMs - pts);
            if (dist > maxDist) {
                maxDist = dist;
                evictIdx = i;
            }
        }

        if (evictIdx == -1) {
            // Every entry is in the protected range: evict the global farthest.
            for (int i = 0; i < m_frames.size(); ++i) {
                int64_t pts = m_frames[i].ptsMs;
                int64_t dist = pts >= keepNearMs ? (pts - keepNearMs) : (keepNearMs - pts);
                if (dist > maxDist) {
                    maxDist = dist;
                    evictIdx = i;
                }
            }
        }

        // Determine the index of the just-inserted frame (it may have shifted
        // if evictIdx < lo because we re-find the inserted entry after eviction).
        // The inserted entry currently sits at lo; if we evict something before
        // lo, the inserted entry will shift down by one.
        bool inserted_was_evicted = (evictIdx == lo);
        m_frames.remove(evictIdx);
        return !inserted_was_evicted;
    }

    return true;
}

// ---------------------------------------------------------------------------
// frameAt
// ---------------------------------------------------------------------------
bool TrackBuffer::frameAt(int64_t playheadMs, FrameHandle& out, int64_t& outPtsMs) const {
    if (m_frames.isEmpty()) return false;

    // upper_bound for playheadMs: first index where ptsMs > playheadMs.
    int lo = 0, hi = m_frames.size();
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (m_frames[mid].ptsMs <= playheadMs)
            lo = mid + 1;
        else
            hi = mid;
    }
    // lo is the first index with ptsMs > playheadMs.
    // The candidate is at lo-1.
    if (lo == 0) return false; // all frames are > playheadMs

    int idx = lo - 1;
    out = m_frames[idx].frame;
    outPtsMs = m_frames[idx].ptsMs;
    return true;
}

// ---------------------------------------------------------------------------
// hasFrameNear
// ---------------------------------------------------------------------------
bool TrackBuffer::hasFrameNear(int64_t targetMs, int64_t toleranceMs) const {
    for (const Frame& fr : m_frames) {
        int64_t diff = fr.ptsMs >= targetMs ? (fr.ptsMs - targetMs) : (targetMs - fr.ptsMs);
        if (diff <= toleranceMs) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// newestPts / oldestPts
// ---------------------------------------------------------------------------
int64_t TrackBuffer::newestPts() const {
    return m_frames.isEmpty() ? -1 : m_frames.last().ptsMs;
}

int64_t TrackBuffer::oldestPts() const {
    return m_frames.isEmpty() ? -1 : m_frames.first().ptsMs;
}

// ---------------------------------------------------------------------------
// trim
// ---------------------------------------------------------------------------
void TrackBuffer::trim(int64_t keepFromMs, int64_t keepToMs) {
    // Remove from the front while ptsMs < keepFromMs.
    while (!m_frames.isEmpty() && m_frames.first().ptsMs < keepFromMs)
        m_frames.removeFirst();

    // Remove from the back while ptsMs > keepToMs.
    while (!m_frames.isEmpty() && m_frames.last().ptsMs > keepToMs)
        m_frames.removeLast();
}
