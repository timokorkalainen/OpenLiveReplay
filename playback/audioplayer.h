#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <QObject>
#include <QAudioSink>
#include <QAudioFormat>
#include <QIODevice>
#include <QMutex>
#include <QByteArray>
#include <QtGlobal>
#include <atomic>

/**
 * Thread-safe FIFO QIODevice feeding QAudioSink in pull mode.
 * PlaybackWorker writes into it from its thread;
 * QAudioSink reads from it on the audio backend thread.
 *
 * readData() always satisfies the full request (padding with silence)
 * so the device clock never stalls.  Silence played in place of due
 * stream data is tracked as "underrun debt": the same number of bytes
 * is dropped from the next push so the stream stays aligned with the
 * device clock instead of drifting later after every underrun.
 */
class AudioRingBuffer : public QIODevice {
    Q_OBJECT
public:
    explicit AudioRingBuffer(QObject *parent = nullptr);

    /// Thread-safe append of PCM data (append + safety cap-trim only).
    void push(const char *data, qint64 len);

    /// Discard all buffered data and reset underrun accounting.
    void clear();

    /// Apply a short fade-out to remaining data, then discard the rest.
    void fadeOutAndClear(int channels);

    /// Safety cap for buffered data (bytes).
    void setMaxBufBytes(int bytes) { m_maxBufBytes = qMax(1, bytes); }

    /// Return and clear the accumulated underrun debt (bytes the device
    /// played as silence in place of due stream data).  Drained by
    /// AudioPlayer::pushSamples so it can repay the debt and fade the splice.
    /// Locks the ring mutex.
    qint64 takeUnderrunDebt();

    /// Add back un-repaid underrun debt so it is carried into the next push.
    /// Called by AudioPlayer::pushSamples when the current payload is smaller
    /// than the outstanding debt (one push repays at most one payload's worth).
    /// Uses ADD semantics so concurrent readData increments on the device
    /// thread are preserved; clamps to m_maxBufBytes consistent with readData.
    /// Locks the ring mutex.
    void addUnderrunDebt(qint64 bytes);

    /// Return and clear the overflow flag.  Set when push() had to trim the
    /// oldest (due-next) bytes because the buffer exceeded the safety cap;
    /// AudioPlayer uses it to force a re-alignment instead of permanently
    /// desyncing.  Locks the ring mutex.
    bool takeOverflowed();

    // QIODevice sequential interface
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    mutable QMutex m_mutex;
    QByteArray m_buf;
    int m_maxBufBytes = 48000 * 2 * 2;  // ~500 ms @ 48 kHz stereo S16 (safety cap)
    qint64 m_underrunDebt = 0;          // silence played in place of due data
    bool m_streamActive = false;        // true once real data flowed since last clear
    bool m_overflowed = false;          // push() cap-trimmed due-next bytes
};

/**
 * AudioPlayer wraps QAudioSink to play back decoded PCM audio
 * from the MKV file.  It uses a thread-safe AudioRingBuffer so
 * that pushSamples() can be called from any thread safely.
 *
 * Pushes are PTS-aligned against the playback master clock:
 *  - the first push after start/clear/unmute is positioned on the
 *    timeline (leading silence inserted, or stale head trimmed),
 *    compensating for the sink's internal buffer latency;
 *  - mid-stream PTS gaps are bridged with silence and overlaps are
 *    trimmed, so imperfectly recorded audio cannot accumulate drift.
 *
 * Thread-safety: all public methods are mutex-protected and can be
 * called from any thread.
 */
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer();

    /// Open the audio device with the given format (call from main thread).
    void start(int sampleRate = 48000, int channels = 2);

    /// Close the audio device.
    void stop();

    /// Push raw interleaved PCM S16LE samples to the ring buffer.
    /// ptsMs is the media timestamp of the first sample; masterTimeMs is
    /// the playback master clock position at push time.
    /// Safe to call from any thread.
    void pushSamples(const uint8_t* data, int numBytes, int64_t ptsMs, int64_t masterTimeMs);

    /// Discard any buffered audio (e.g. on seek) and re-arm PTS alignment.
    void clear();

    /// Mute / unmute.  When muted, pushSamples silently discards data.
    void setMuted(bool muted);
    bool isMuted() const;

    /// Number of times clear() has been called (incremented under m_mutex).
    int clearCount() const { return m_clearCount; }

    /// Set output-device latency compensation (ms, clamped 0..kMaxOutputLatencyMs).
    /// Folded into the playout latency AND the resync threshold (which scales with
    /// it, so a high offset does not storm re-aligns). Takes effect on the next push;
    /// for an already-aligned live stream the caller should clear() to re-align now.
    void setOutputLatencyOffsetMs(int ms) {
        m_outputLatencyOffsetMs.store(qBound(0, ms, kMaxOutputLatencyMs),
                                      std::memory_order_relaxed);
    }
    /// Number of times the aligned branch re-aligned (incremented under m_mutex in
    /// pushSamples). Used by the e2e to assert no re-align storm under a high offset.
    int resyncCount() const { return m_resyncCount; }

private:
    QAudioSink*      m_sink = nullptr;
    AudioRingBuffer* m_ringBuffer = nullptr;
    mutable QMutex   m_mutex;
    bool m_muted   = true;   // start muted until a single view is selected
    bool m_started = false;
    int  m_sampleRate = 48000;
    int  m_channels   = 2;
    qint64 m_sinkLatencyBytes = 0;   // granted sink buffer size (always full)
    bool m_aligned = false;          // stream start aligned to master clock
    int64_t m_expectedNextPtsSamples = 0;  // continuity tracking (sample frames)
    int m_fadeInRemaining = 0;             // FRAMES remaining in fade-in ramp
    int m_fadeInLen = 0;                   // FRAMES in the current fade-in ramp (the
                                           // ramp denominator; set when the fade is armed)
    static constexpr int kFadeInSamples = 480;   // 10 ms @ 48 kHz
    static constexpr int kSinkBufferMs  = 60;    // device-side latency budget
    static constexpr int kRingCapMs     = 500;   // ring safety cap
    static constexpr int kJitterTolMs   = 30;    // PTS continuity tolerance
    static constexpr int kSpliceFadeSamples = 120; // 2.5 ms de-click ramp after a splice
    static constexpr int kMaxOutputLatencyMs = 500; // user offset ceiling
    static constexpr int kResyncHeadroomMs = 250;   // genuine-desync margin ABOVE the
                                                    // expected steady-state offset divergence;
                                                    // resync trigger = kResyncHeadroomMs + offset
    // Extra output latency (ms) beyond the QAudioSink buffer to compensate
    // for when aligning the stream start.  Qt's QAudioSink exposes NO API to
    // query real hardware/driver/Bluetooth output latency (typically
    // 100-300 ms for BT), so exact automatic compensation is impossible.
    // Driven by a user-facing "audio offset" setting via setOutputLatencyOffsetMs().
    // The resync threshold scales with this offset so a high offset does not
    // storm re-aligns in the aligned branch.
    int m_clearCount = 0;
    // Output-device latency compensation (ms, 0..kMaxOutputLatencyMs). Folded into
    // the playout latency AND the resync threshold (which scales with it). Written
    // by the UI thread, read on the push path — relaxed: standalone scalar.
    std::atomic<int> m_outputLatencyOffsetMs{0};
    int m_resyncCount = 0; // times the aligned branch re-aligned (push thread)
};

#endif // AUDIOPLAYER_H
