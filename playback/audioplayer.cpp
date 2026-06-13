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

    // Plain append + safety cap-trim.  Underrun-debt repayment now lives in
    // AudioPlayer::pushSamples (so the splice can be faded); this method does
    // not touch m_underrunDebt.
    m_buf.append(data, len);
    m_streamActive = true;

    // Safety cap to avoid unbounded growth.  Should not trigger in normal
    // operation: the producer paces itself against the master clock.  Trimming
    // the OLDEST (due-next) bytes shifts the stream early with no alignment
    // adjustment, so flag it: AudioPlayer will force a re-align on the next
    // push instead of compounding a permanent desync.
    if (m_buf.size() > m_maxBufBytes) {
        m_buf.remove(0, m_buf.size() - m_maxBufBytes);
        m_overflowed = true;
    }
}

void AudioRingBuffer::clear() {
    QMutexLocker locker(&m_mutex);
    m_buf.clear();
    m_underrunDebt = 0;
    m_streamActive = false;
    m_overflowed = false;
}

void AudioRingBuffer::fadeOutAndClear(int channels) {
    QMutexLocker locker(&m_mutex);
    m_underrunDebt = 0;
    m_streamActive = false;
    m_overflowed = false;
    if (m_buf.isEmpty()) return;

    // Keep at most ~5 ms of audio for the fade-out tail (240 samples @ 48 kHz)
    const int kFadeSamples = 240 * channels;  // per-channel pairs
    const int kFadeBytes   = kFadeSamples * int(sizeof(int16_t));

    if (m_buf.size() > kFadeBytes)
        m_buf = m_buf.left(kFadeBytes);  // trim to just what's next to play

    int16_t* samples = reinterpret_cast<int16_t*>(m_buf.data());
    int nSamples = m_buf.size() / int(sizeof(int16_t));
    // Ramp per channel-FRAME so both channels of a stereo frame get the same
    // gain (otherwise L and R diverge by 1/nFrames across the tail).
    const int ch = qMax(1, channels);
    const int nFrames = nSamples / ch;
    for (int i = 0; i < nSamples; ++i) {
        const int frame = i / ch;
        const double gain = (nFrames > 0) ? 1.0 - double(frame) / double(nFrames) : 0.0;
        samples[i] = int16_t(samples[i] * gain);
    }
}

qint64 AudioRingBuffer::takeUnderrunDebt() {
    QMutexLocker locker(&m_mutex);
    const qint64 debt = m_underrunDebt;
    m_underrunDebt = 0;
    return debt;
}

void AudioRingBuffer::addUnderrunDebt(qint64 bytes) {
    if (bytes <= 0) return;
    QMutexLocker locker(&m_mutex);
    m_underrunDebt += bytes;
    // Cap to one ring's worth (consistent with readData's cap) so a large stall
    // cannot grow the carried-forward debt beyond ~500 ms of audio.
    if (m_underrunDebt > m_maxBufBytes) {
        m_underrunDebt = m_maxBufBytes;
    }
}

bool AudioRingBuffer::takeOverflowed() {
    QMutexLocker locker(&m_mutex);
    const bool f = m_overflowed;
    m_overflowed = false;
    return f;
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
            // Cap the debt at one ring's worth so a long stall (e.g. a pause
            // with a stray late push leaving m_streamActive true) can swallow
            // at most ~500 ms of real audio on resume, not the whole pause.
            if (m_underrunDebt > m_maxBufBytes) {
                m_underrunDebt = m_maxBufBytes;
            }
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

    // If the ring had to cap-trim due-next bytes since the last push, the
    // stream shifted early with no alignment fix.  Force a re-align so this
    // push re-anchors to the master clock instead of compounding the desync.
    if (m_ringBuffer->takeOverflowed()) {
        m_aligned = false;
    }

    const int bytesPerFrame = m_channels * int(sizeof(int16_t));
    const int nFrames = numBytes / bytesPerFrame;
    if (nFrames <= 0) return;

    const int64_t ptsSamples = ptsMs * m_sampleRate / 1000;
    const int64_t jitterTolSamples = int64_t(kJitterTolMs) * m_sampleRate / 1000;

    // Playout latency = the always-full sink buffer plus a manual offset for
    // hardware/driver/Bluetooth output latency Qt cannot report (see header).
    const int bytesPerSecond = m_sampleRate * m_channels * int(sizeof(int16_t));
    const int64_t latencySamples =
        (m_sinkLatencyBytes + int64_t(kOutputLatencyOffsetMs) * bytesPerSecond / 1000)
        / bytesPerFrame;
    const int64_t dueSamples = masterTimeMs * m_sampleRate / 1000 + latencySamples;

    const char* payload = reinterpret_cast<const char*>(data);
    qint64 payloadBytes = qint64(nFrames) * bytesPerFrame;
    qint64 silencePrefixBytes = 0;
    bool spliced = false;  // true if this push jumps the waveform → needs a fade

    // Defensive self-heal: if a seek moved the playhead without an
    // AudioPlayer::clear(), the aligned branch (which tracks PTS continuity
    // only) would stay mis-positioned forever.  Detect a large divergence from
    // the master clock and re-align.  The threshold is far beyond the jitter
    // tolerance, so normal 1x play (ptsMs ≈ playhead) never trips it.
    if (m_aligned) {
        const int64_t resyncSamples = int64_t(kResyncThresholdMs) * m_sampleRate / 1000;
        if (qAbs(ptsSamples - dueSamples) > resyncSamples) {
            m_aligned = false;
        }
    }

    if (!m_aligned) {
        // Position the stream start on the timeline.  The sink's internal
        // buffer is always full (silence-padded reads), so data pushed now
        // starts playing ~latency later: schedule against that.
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
            // Overlap: drop the part that already played.  This splices the
            // waveform mid-stream, so fade the remainder in to avoid a click.
            const int64_t trimFrames = qMin<int64_t>(-delta, nFrames);
            payload      += trimFrames * bytesPerFrame;
            payloadBytes -= trimFrames * bytesPerFrame;
            spliced = true;
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

    // Repay underrun debt here (moved out of AudioRingBuffer::push): the
    // device already played this many bytes as silence, so skip them from the
    // payload head to stay aligned with the device clock.  Draining the debt
    // under the ring mutex (takeUnderrunDebt) matches readData's accrual under
    // the same mutex — no double-count, no loss.
    // If the payload is smaller than the total debt, the leftover is written
    // back via addUnderrunDebt so it is repaid on the next push — one push
    // repays at most one payload's worth, keeping debt reduction frame-aligned.
    const qint64 debt = m_ringBuffer->takeUnderrunDebt();
    if (debt > 0) {
        const qint64 drop = qMin(debt, payloadBytes);
        if (debt - drop > 0) {
            m_ringBuffer->addUnderrunDebt(debt - drop);  // carry leftover into next push
        }
        if (drop > 0) {
            payload      += drop;
            payloadBytes -= drop;
            spliced = true;  // skipping to a mid-waveform sample → fade it in
            if (payloadBytes <= 0) {
                // Debt consumed the whole payload; nothing to enqueue but the
                // continuity position still advances past these samples.
                return;
            }
        }
    }

    // Whenever the waveform was spliced (debt-skip or overlap-trim), arm a
    // short de-click fade so the post-splice payload ramps in instead of
    // jumping from device-played silence to a mid-waveform sample.
    // m_fadeInRemaining / m_fadeInLen count FRAMES (see fade-in loop below).
    // If a longer fade (clear/unmute) is already in flight, keep the shorter
    // splice fade so we don't over-attenuate; otherwise arm a fresh one.
    if (spliced) {
        if (m_fadeInRemaining <= 0 || kSpliceFadeSamples < m_fadeInRemaining) {
            m_fadeInRemaining = kSpliceFadeSamples;
            m_fadeInLen       = kSpliceFadeSamples;
        }
    }

    // Apply a short linear fade-in ramp (after unmute / view switch / splice)
    // to eliminate the click from an abrupt sample transition.  The ramp is
    // indexed per channel-FRAME (m_fadeInRemaining counts frames) so both
    // channels of a stereo frame share one gain — otherwise L and R would
    // diverge by 1/ramp-length across the ramp.  m_fadeInLen is the ramp
    // denominator so the gain always starts at ~0 regardless of ramp length.
    if (m_fadeInRemaining > 0 && m_fadeInLen > 0) {
        // Work on a mutable copy so we don't alter the caller's buffer
        QByteArray tmp(payload, payloadBytes);
        int16_t* samples = reinterpret_cast<int16_t*>(tmp.data());
        int totalSamples = int(payloadBytes) / int(sizeof(int16_t));
        for (int i = 0; i < totalSamples && m_fadeInRemaining > 0; ++i) {
            const int rampPos = m_fadeInLen - m_fadeInRemaining;
            const double gain = double(rampPos) / double(m_fadeInLen);
            samples[i] = int16_t(samples[i] * gain);
            // Advance the ramp once per frame so all channels share the gain.
            if ((i % m_channels) == (m_channels - 1)) {
                --m_fadeInRemaining;
            }
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
    // Re-arm timeline alignment and fade-in for the next pushed samples.
    // m_fadeInRemaining / m_fadeInLen count FRAMES (kFadeInSamples = 10 ms).
    m_aligned = false;
    m_fadeInRemaining = kFadeInSamples;
    m_fadeInLen       = kFadeInSamples;
}

void AudioPlayer::setMuted(bool muted) {
    QMutexLocker locker(&m_mutex);
    bool wasM = m_muted;
    m_muted = muted;
    if (muted && m_ringBuffer) {
        m_ringBuffer->clear();
    }
    // Arm fade-in ramp and re-alignment when transitioning muted → unmuted.
    // m_fadeInRemaining / m_fadeInLen count FRAMES (kFadeInSamples = 10 ms).
    if (wasM && !muted) {
        m_fadeInRemaining = kFadeInSamples;
        m_fadeInLen       = kFadeInSamples;
        m_aligned = false;
        if (m_ringBuffer) m_ringBuffer->clear();
    }
}

bool AudioPlayer::isMuted() const {
    QMutexLocker locker(&m_mutex);
    return m_muted;
}
