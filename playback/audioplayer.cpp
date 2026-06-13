#include "audioplayer.h"
#include <QMediaDevices>
#include <QAudioDevice>
#include <QDebug>

// ─── AudioRingBuffer ───────────────────────────────────────────────────

AudioRingBuffer::AudioRingBuffer(QObject *parent)
    : QIODevice(parent)
{
    open(QIODevice::ReadOnly);
}

void AudioRingBuffer::push(const char *data, qint64 len) {
    if (len <= 0) return;
    QMutexLocker locker(&m_mutex);

    // Repay underrun debt: the device already played silence in place of
    // this data, so skip the equivalent amount to stay aligned with the
    // device clock instead of drifting later after every underrun.
    if (m_underrunDebt > 0) {
        const qint64 drop = qMin(m_underrunDebt, len);
        data += drop;
        len  -= drop;
        m_underrunDebt -= drop;
        if (len <= 0) return;
    }

    m_buf.append(data, len);
    m_streamActive = true;

    // Safety cap to avoid unbounded growth.  Should not trigger in normal
    // operation: the producer paces itself against the master clock.
    if (m_buf.size() > m_maxBufBytes) {
        m_buf.remove(0, m_buf.size() - m_maxBufBytes);
    }
}

void AudioRingBuffer::clear() {
    QMutexLocker locker(&m_mutex);
    m_buf.clear();
    m_underrunDebt = 0;
    m_streamActive = false;
}

void AudioRingBuffer::fadeOutAndClear(int channels) {
    QMutexLocker locker(&m_mutex);
    m_underrunDebt = 0;
    m_streamActive = false;
    if (m_buf.isEmpty()) return;

    // Keep at most ~5 ms of audio for the fade-out tail (240 samples @ 48 kHz)
    const int kFadeSamples = 240 * channels;  // per-channel pairs
    const int kFadeBytes   = kFadeSamples * int(sizeof(int16_t));

    if (m_buf.size() > kFadeBytes)
        m_buf = m_buf.left(kFadeBytes);  // trim to just what's next to play

    int16_t* samples = reinterpret_cast<int16_t*>(m_buf.data());
    int nSamples = m_buf.size() / int(sizeof(int16_t));
    for (int i = 0; i < nSamples; ++i) {
        double gain = 1.0 - double(i) / double(nSamples);
        samples[i] = int16_t(samples[i] * gain);
    }
}

qint64 AudioRingBuffer::bytesAvailable() const {
    QMutexLocker locker(&m_mutex);
    return m_buf.size() + QIODevice::bytesAvailable();
}

qint64 AudioRingBuffer::readData(char *data, qint64 maxSize) {
    QMutexLocker locker(&m_mutex);
    const qint64 toRead = qMin(maxSize, (qint64)m_buf.size());
    if (toRead > 0) {
        memcpy(data, m_buf.constData(), toRead);
        m_buf.remove(0, toRead);
    }
    if (toRead < maxSize) {
        // Pad with silence so the device clock never stalls.  While a
        // stream is active this is an underrun: record it so the next
        // push skips the bytes whose play time this silence consumed.
        memset(data + toRead, 0, maxSize - toRead);
        if (m_streamActive) {
            m_underrunDebt += maxSize - toRead;
        }
    }
    return maxSize;
}

qint64 AudioRingBuffer::writeData(const char *, qint64) {
    return -1; // read-only device
}

// ─── AudioPlayer ───────────────────────────────────────────────────────

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent) {}

AudioPlayer::~AudioPlayer() {
    QMutexLocker locker(&m_mutex);
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    delete m_ringBuffer;
    m_ringBuffer = nullptr;
    m_started = false;
}

void AudioPlayer::start(int sampleRate, int channels) {
    QMutexLocker locker(&m_mutex);

    // Tear down previous instance
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    delete m_ringBuffer;
    m_ringBuffer = nullptr;
    m_started  = false;

    m_sampleRate = sampleRate;
    m_channels   = channels;

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isFormatSupported(format)) {
        qWarning() << "AudioPlayer: requested format not supported by default device";
        return;
    }

    const int bytesPerSecond = sampleRate * channels * int(sizeof(int16_t));

    m_ringBuffer = new AudioRingBuffer(this);
    m_ringBuffer->setMaxBufBytes(bytesPerSecond * kRingCapMs / 1000);

    m_sink = new QAudioSink(device, format, this);
    // Small device-side buffer keeps latency bounded; the ring buffer
    // absorbs producer jitter.
    m_sink->setBufferSize(bytesPerSecond * kSinkBufferMs / 1000);
    m_sink->start(m_ringBuffer);   // pull mode: sink reads from ring buffer

    // The sink keeps its buffer topped up (readData always satisfies the
    // full request), so the granted buffer size is a constant playout
    // latency we compensate for when aligning the stream start.
    m_sinkLatencyBytes = m_sink->bufferSize();

    m_aligned = false;
    m_started = true;

    qDebug() << "AudioPlayer: started" << sampleRate << "Hz," << channels
             << "ch (pull mode), sink buffer" << m_sinkLatencyBytes << "bytes";
}

void AudioPlayer::stop() {
    QMutexLocker locker(&m_mutex);
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    delete m_ringBuffer;
    m_ringBuffer = nullptr;
    m_started  = false;
    m_aligned  = false;
    m_sinkLatencyBytes = 0;
}

void AudioPlayer::pushSamples(const uint8_t *data, int numBytes,
                              int64_t ptsMs, int64_t masterTimeMs) {
    QMutexLocker locker(&m_mutex);
    if (!m_started || m_muted || !m_ringBuffer || numBytes <= 0) return;

    const int bytesPerFrame = m_channels * int(sizeof(int16_t));
    const int nFrames = numBytes / bytesPerFrame;
    if (nFrames <= 0) return;

    const int64_t ptsSamples = ptsMs * m_sampleRate / 1000;
    const int64_t jitterTolSamples = int64_t(kJitterTolMs) * m_sampleRate / 1000;
    const char* payload = reinterpret_cast<const char*>(data);
    qint64 payloadBytes = qint64(nFrames) * bytesPerFrame;
    qint64 silencePrefixBytes = 0;

    if (!m_aligned) {
        // Position the stream start on the timeline.  The sink's internal
        // buffer is always full (silence-padded reads), so data pushed now
        // starts playing ~sinkLatency later: schedule against that.
        const int64_t latencySamples = m_sinkLatencyBytes / bytesPerFrame;
        const int64_t dueSamples = masterTimeMs * m_sampleRate / 1000 + latencySamples;
        const int64_t lead = ptsSamples - dueSamples;
        if (lead + nFrames <= 0) {
            return;  // entirely in the past; wait for newer data
        }
        if (lead > 0) {
            silencePrefixBytes = qMin<int64_t>(lead, 2 * m_sampleRate) * bytesPerFrame;
        } else if (lead < 0) {
            const qint64 trimBytes = qint64(-lead) * bytesPerFrame;
            payload      += trimBytes;
            payloadBytes -= trimBytes;
        }
        m_aligned = true;
    } else {
        const int64_t delta = ptsSamples - m_expectedNextPtsSamples;
        if (delta > jitterTolSamples) {
            // Gap in the recording: bridge with silence to hold sync.
            silencePrefixBytes = qMin<int64_t>(delta, 2 * m_sampleRate) * bytesPerFrame;
        } else if (delta < -jitterTolSamples) {
            // Overlap: drop the part that already played.
            const int64_t trimFrames = qMin<int64_t>(-delta, nFrames);
            payload      += trimFrames * bytesPerFrame;
            payloadBytes -= trimFrames * bytesPerFrame;
            if (payloadBytes <= 0) {
                m_expectedNextPtsSamples = ptsSamples + nFrames;
                return;
            }
        }
    }
    m_expectedNextPtsSamples = ptsSamples + nFrames;

    if (silencePrefixBytes > 0) {
        const QByteArray silence(int(silencePrefixBytes), '\0');
        m_ringBuffer->push(silence.constData(), silence.size());
    }

    // Apply a short linear fade-in ramp after unmute / view switch
    // to eliminate the click from an abrupt sample transition.
    if (m_fadeInRemaining > 0) {
        // Work on a mutable copy so we don't alter the caller's buffer
        QByteArray tmp(payload, payloadBytes);
        int16_t* samples = reinterpret_cast<int16_t*>(tmp.data());
        int totalSamples = int(payloadBytes) / int(sizeof(int16_t));
        for (int i = 0; i < totalSamples && m_fadeInRemaining > 0; ++i, --m_fadeInRemaining) {
            // Per-channel sample index in the fade ramp
            int rampPos = kFadeInSamples * m_channels - m_fadeInRemaining;
            double gain = double(rampPos) / double(kFadeInSamples * m_channels);
            samples[i] = int16_t(samples[i] * gain);
        }
        m_ringBuffer->push(tmp.constData(), tmp.size());
    } else {
        m_ringBuffer->push(payload, payloadBytes);
    }
}

void AudioPlayer::clear() {
    QMutexLocker locker(&m_mutex);
    ++m_clearCount;
    if (m_ringBuffer) {
        m_ringBuffer->fadeOutAndClear(m_channels);
    }
    // Re-arm timeline alignment and fade-in for the next pushed samples
    m_aligned = false;
    m_fadeInRemaining = kFadeInSamples * m_channels;
}

void AudioPlayer::setMuted(bool muted) {
    QMutexLocker locker(&m_mutex);
    bool wasM = m_muted;
    m_muted = muted;
    if (muted && m_ringBuffer) {
        m_ringBuffer->clear();
    }
    // Arm fade-in ramp and re-alignment when transitioning muted → unmuted
    if (wasM && !muted) {
        m_fadeInRemaining = kFadeInSamples * m_channels;
        m_aligned = false;
        if (m_ringBuffer) m_ringBuffer->clear();
    }
}

bool AudioPlayer::isMuted() const {
    QMutexLocker locker(&m_mutex);
    return m_muted;
}
