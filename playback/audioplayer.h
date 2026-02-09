#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <QObject>
#include <QAudioSink>
#include <QAudioFormat>
#include <QIODevice>
#include <QMutex>
#include <QByteArray>

/**
 * Thread-safe ring-buffer QIODevice.
 * PlaybackWorker writes into it from its thread;
 * QAudioSink reads from it on the main / audio thread.
 */
class AudioRingBuffer : public QIODevice {
    Q_OBJECT
public:
    explicit AudioRingBuffer(QObject *parent = nullptr);

    /// Thread-safe append of PCM data.
    void push(const char *data, qint64 len);

    /// Discard all buffered data.
    void clear();

    // QIODevice sequential interface
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    mutable QMutex m_mutex;
    QByteArray m_buf;
    static constexpr int kMaxBufBytes = 48000 * 2 * 2; // ~500 ms @ 48 kHz stereo S16
};

/**
 * AudioPlayer wraps QAudioSink to play back decoded PCM audio
 * from the MKV file.  It uses a thread-safe AudioRingBuffer so
 * that pushSamples() can be called from any thread safely.
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
    /// Safe to call from any thread.
    void pushSamples(const uint8_t* data, int numBytes);

    /// Discard any buffered audio (e.g. on seek).
    void clear();

    /// Mute / unmute.  When muted, pushSamples silently discards data.
    void setMuted(bool muted);
    bool isMuted() const;

private:
    QAudioSink*      m_sink = nullptr;
    AudioRingBuffer* m_ringBuffer = nullptr;
    mutable QMutex   m_mutex;
    bool m_muted   = true;   // start muted until a single view is selected
    bool m_started = false;
    int  m_sampleRate = 48000;
    int  m_channels   = 2;
};

#endif // AUDIOPLAYER_H
