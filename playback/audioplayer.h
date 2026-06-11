#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <QObject>
#include <QAudioSink>
#include <QAudioFormat>
#include <QIODevice>
#include <QMutex>
#include <QByteArray>

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

    /// Thread-safe append of PCM data.
    void push(const char *data, qint64 len);

    /// Discard all buffered data and reset underrun accounting.
    void clear();

    /// Apply a short fade-out to remaining data, then discard the rest.
    void fadeOutAndClear(int channels);

    /// Safety cap for buffered data (bytes).
    void setMaxBufBytes(int bytes) { m_maxBufBytes = qMax(1, bytes); }

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
    int  m_fadeInRemaining = 0;      // samples remaining in fade-in ramp
    static constexpr int kFadeInSamples = 480;   // 10 ms @ 48 kHz
    static constexpr int kSinkBufferMs  = 60;    // device-side latency budget
    static constexpr int kRingCapMs     = 500;   // ring safety cap
    static constexpr int kJitterTolMs   = 30;    // PTS continuity tolerance
    int m_clearCount = 0;
};

#endif // AUDIOPLAYER_H
