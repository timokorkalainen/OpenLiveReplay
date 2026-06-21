#ifndef AUDIOFRAMEQUEUE_H
#define AUDIOFRAMEQUEUE_H
#include <QByteArray>
#include <QQueue>
#include <cstdint>

// Holds decoded PCM audio frames (interleaved S16 stereo) tagged with their
// media PTS, and releases them paced against the playhead. Decouples the
// audio push position from the video decode window (spec §6.7): the video
// window may lead ~500ms, but only frames within kAudioLeadMs of the
// playhead are released into the (500ms-capped) AudioPlayer ring.
class AudioFrameQueue {
public:
    struct Frame {
        int64_t ptsMs{};
        QByteArray pcm;
    };

    void enqueue(int64_t ptsMs, const char* data, int bytes);

    // Pop the next frame iff its PTS <= playheadMs + leadMs. Returns false
    // when nothing is due. FIFO by enqueue order (== PTS order in normal play).
    bool releaseDue(int64_t playheadMs, int64_t leadMs, Frame& out);

    // Drop everything older than (playheadMs - keepBehindMs): bounds memory.
    void dropOlderThan(int64_t playheadMs, int64_t keepBehindMs);

    void clear();
    bool isEmpty() const { return m_q.isEmpty(); }
    int spanMs() const; // newest.pts - oldest.pts, 0 if <2

private:
    QQueue<Frame> m_q;
};
#endif
