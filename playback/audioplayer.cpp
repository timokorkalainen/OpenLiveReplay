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
    m_buf.append(data, len);
    // Cap to ~500 ms to avoid unbounded growth
    if (m_buf.size() > kMaxBufBytes) {
        m_buf.remove(0, m_buf.size() - kMaxBufBytes);
    }
}

void AudioRingBuffer::clear() {
    QMutexLocker locker(&m_mutex);
    m_buf.clear();
}

qint64 AudioRingBuffer::bytesAvailable() const {
    QMutexLocker locker(&m_mutex);
    return m_buf.size() + QIODevice::bytesAvailable();
}

qint64 AudioRingBuffer::readData(char *data, qint64 maxSize) {
    QMutexLocker locker(&m_mutex);
    qint64 toRead = qMin(maxSize, (qint64)m_buf.size());
    if (toRead <= 0) {
        // Return silence so the sink doesn't stall
        memset(data, 0, maxSize);
        return maxSize;
    }
    memcpy(data, m_buf.constData(), toRead);
    m_buf.remove(0, toRead);
    // Fill remainder with silence
    if (toRead < maxSize) {
        memset(data + toRead, 0, maxSize - toRead);
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

    m_ringBuffer = new AudioRingBuffer(this);

    m_sink = new QAudioSink(device, format, this);
    // ~200 ms internal buffer keeps latency low while avoiding underruns
    m_sink->setBufferSize(sampleRate * channels * int(sizeof(int16_t)) / 5);
    m_sink->start(m_ringBuffer);   // pull mode: sink reads from ring buffer
    m_started = true;

    qDebug() << "AudioPlayer: started" << sampleRate << "Hz," << channels << "ch (pull mode)";
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
}

void AudioPlayer::pushSamples(const uint8_t *data, int numBytes) {
    QMutexLocker locker(&m_mutex);
    if (!m_started || m_muted || !m_ringBuffer || numBytes <= 0) return;
    m_ringBuffer->push(reinterpret_cast<const char*>(data), numBytes);
}

void AudioPlayer::clear() {
    QMutexLocker locker(&m_mutex);
    if (m_ringBuffer) {
        m_ringBuffer->clear();
    }
}

void AudioPlayer::setMuted(bool muted) {
    QMutexLocker locker(&m_mutex);
    m_muted = muted;
    if (muted && m_ringBuffer) {
        m_ringBuffer->clear();
    }
}

bool AudioPlayer::isMuted() const {
    QMutexLocker locker(&m_mutex);
    return m_muted;
}
