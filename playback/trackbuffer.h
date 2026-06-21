#ifndef TRACKBUFFER_H
#define TRACKBUFFER_H
#include "playback/output/framehandle.h"

#include <QVector>
#include <cstdint>

// One video track's decoded-frame window. Frames are kept sorted ascending
// by PTS and unique by PTS. Pure data structure: no ffmpeg, no threads.
// The owner serializes access (PlaybackWorker::m_bufferMutex).
class TrackBuffer {
public:
    struct Frame {
        int64_t ptsMs = -1;
        FrameHandle frame;
    };

    // Insert sorted, unique by PTS (existing PTS is replaced). Enforces the
    // frame cap by evicting the entry farthest (in PTS) from keepNearMs,
    // but never one whose PTS is in [keepNearMs, protectToMs] (the live
    // fill edge). Returns false if the frame was dropped by the cap.
    bool insert(int64_t ptsMs, const FrameHandle& f, int capFrames, int64_t keepNearMs,
                int64_t protectToMs);

    // The frame to display at playhead: largest PTS <= playheadMs.
    // Returns false (out untouched) if no such frame.
    bool frameAt(int64_t playheadMs, FrameHandle& out, int64_t& outPtsMs) const;
    QVector<Frame> framesSnapshot() const { return m_frames; }

    // True iff a frame exists within +/- toleranceMs of targetMs.
    bool hasFrameNear(int64_t targetMs, int64_t toleranceMs) const;

    int64_t newestPts() const; // -1 if empty
    int64_t oldestPts() const; // -1 if empty
    bool isEmpty() const { return m_frames.isEmpty(); }
    int size() const { return static_cast<int>(m_frames.size()); }

    // Drop frames with PTS < keepFromMs or PTS > keepToMs.
    void trim(int64_t keepFromMs, int64_t keepToMs);
    void clear() { m_frames.clear(); }

private:
    QVector<Frame> m_frames; // sorted ascending by ptsMs, unique
};
#endif
