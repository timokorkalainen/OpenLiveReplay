#include "frameprovider.h"
#include "playback/output/qtpreviewsink.h"
#include <QtCore/qdebug.h>
#include <QMetaObject>
#include <QElapsedTimer>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <memory>

namespace {
struct FlushWaitState {
    QMutex mutex;
    QWaitCondition condition;
    bool done = false;
    bool success = false;
};

QVideoFrame frameForDisplayTime(const QVideoFrame& frame, qint64 startUs) {
    QVideoFrame displayFrame = frame;
    displayFrame.setStartTime(startUs);
    displayFrame.setEndTime(-1);
    return displayFrame;
}
} // namespace

FrameProvider::FrameProvider(QObject *parent)
    : QObject(parent)
{
    m_displayTimer.start();
}

QVideoSink* FrameProvider::videoSink() const
{
    return m_sink;
}


void FrameProvider::setVideoSink(QVideoSink *sink)
{
    if (m_sink == sink) return;

    QPointer<QVideoSink> oldSink = m_sink;
    m_sink = sink;

    if (oldSink) {
        removeVideoSink(oldSink);
    }

    if (m_sink) {
        addVideoSink(m_sink);
    }

    emit videoSinkChanged();
    qDebug() << "C++: Video Sink successfully connected from QML";
}

void FrameProvider::addVideoSink(QVideoSink *sink)
{
    if (!sink) return;

    {
        QMutexLocker locker(&m_sinkMutex);
        for (const auto &existing : m_sinks) {
            if (existing == sink) return;
        }
        m_sinks.append(sink);
    }

    connect(sink, &QObject::destroyed, this, [this, sink]() {
        removeVideoSink(sink);
    });

    QVideoFrame lastFrameCopy;
    quint64 frameSerial = 0;
    {
        QMutexLocker locker(&m_frameMutex);
        lastFrameCopy = m_lastFrame;
        frameSerial = m_frameSerial;
    }
    if (lastFrameCopy.isValid()) {
        if (sink->thread() == QThread::currentThread()) {
            sink->setVideoFrame(lastFrameCopy);
            markSinkAppliedSerial(sink, frameSerial);
        } else {
            QPointer<FrameProvider> providerPtr = this;
            QPointer<QVideoSink> sinkPtr = sink;
            QMetaObject::invokeMethod(
                sink,
                [providerPtr, sinkPtr, frameSerial]() mutable {
                    if (!providerPtr || !sinkPtr) return;
                    QVideoFrame latest;
                    if (!providerPtr->latestFrameForSerial(frameSerial, &latest)) return;
                    if (sinkPtr) {
                        sinkPtr->setVideoFrame(latest);
                        providerPtr->markSinkAppliedSerial(sinkPtr, frameSerial);
                    }
                },
                Qt::QueuedConnection);
        }
    }
}

void FrameProvider::removeVideoSink(QVideoSink *sink)
{
    if (!sink) return;

    QMutexLocker locker(&m_sinkMutex);
    for (qsizetype i = m_sinks.size() - 1; i >= 0; --i) {
        if (!m_sinks[i] || m_sinks[i] == sink) {
            m_sinks.removeAt(i);
        }
    }
    m_appliedSerialBySink.remove(sink);

    if (m_sink == sink) {
        m_sink = nullptr;
    }
}

quint64 FrameProvider::deliverFrame(const QVideoFrame& frame) {
    quint64 frameSerial = 0;
    QVideoFrame displayFrame;
    {
        QMutexLocker locker(&m_frameMutex);
        frameSerial = ++m_frameSerial;
        displayFrame = frameForDisplayTime(frame, nextDisplayStartUsLocked());
        m_lastFrame = displayFrame;
    }
    emit frameChanged(frameSerial);

    QList<QPointer<QVideoSink>> sinksCopy;
    {
        QMutexLocker locker(&m_sinkMutex);
        sinksCopy = m_sinks;
    }

    if (sinksCopy.isEmpty()) return frameSerial;

    for (const auto &sink : sinksCopy) {
        if (!sink) continue;
        if (sink->thread() == QThread::currentThread()) {
            sink->setVideoFrame(displayFrame);
            markSinkAppliedSerial(sink, frameSerial);
        } else {
            QPointer<FrameProvider> providerPtr = this;
            QPointer<QVideoSink> sinkPtr = sink;
            QMetaObject::invokeMethod(
                sink,
                [providerPtr, sinkPtr, frameSerial]() mutable {
                    if (!providerPtr || !sinkPtr) return;
                    QVideoFrame latest;
                    if (!providerPtr->latestFrameForSerial(frameSerial, &latest)) return;
                    if (sinkPtr) {
                        sinkPtr->setVideoFrame(latest);
                        providerPtr->markSinkAppliedSerial(sinkPtr, frameSerial);
                    }
                },
                Qt::QueuedConnection);
        }
    }
    return frameSerial;
}

quint64 FrameProvider::deliverHandle(const FrameHandle& handle) {
    const QVideoFrame frame = QtPreviewSink::toQVideoFrame(handle);
    if (!frame.isValid()) return 0;
    return deliverFrame(frame);
}

bool FrameProvider::flushVideoSinks(int timeoutMs, quint64 minSerial) const {
    QList<QPointer<QVideoSink>> sinksCopy;
    {
        QMutexLocker locker(&m_sinkMutex);
        sinksCopy = m_sinks;
    }
    if (sinksCopy.isEmpty()) return true;

    const int boundedTimeoutMs = std::max(0, timeoutMs);
    QElapsedTimer timer;
    timer.start();

    for (const auto& sink : sinksCopy) {
        if (!sink) continue;
        if (minSerial > 0 && sinkAppliedSerial(sink) >= minSerial) continue;
        if (sink->thread() == QThread::currentThread()) {
            if (minSerial == 0 || sinkAppliedSerial(sink) >= minSerial) continue;
            return false;
        }

        const qint64 elapsedMs = timer.elapsed();
        const int remainingMs = int(std::max<qint64>(0, qint64(boundedTimeoutMs) - elapsedMs));
        if (remainingMs <= 0) return false;

        auto state = std::make_shared<FlushWaitState>();
        QPointer<FrameProvider> providerPtr = const_cast<FrameProvider*>(this);
        QPointer<QVideoSink> sinkPtr = sink;
        const bool queued = QMetaObject::invokeMethod(
            sink,
            [providerPtr, sinkPtr, minSerial, state]() {
                QMutexLocker locker(&state->mutex);
                state->success =
                    minSerial == 0 || (providerPtr && sinkPtr &&
                                       providerPtr->sinkAppliedSerial(sinkPtr) >= minSerial);
                state->done = true;
                state->condition.wakeAll();
            },
            Qt::QueuedConnection);
        if (!queued) return false;

        QMutexLocker locker(&state->mutex);
        while (!state->done) {
            const qint64 waitElapsedMs = timer.elapsed();
            const int waitRemainingMs =
                int(std::max<qint64>(0, qint64(boundedTimeoutMs) - waitElapsedMs));
            if (waitRemainingMs <= 0 || !state->condition.wait(&state->mutex, waitRemainingMs)) {
                return false;
            }
        }
        if (!state->success) return false;
    }

    return true;
}

void FrameProvider::addDirectPreviewConsumer(QObject* consumer) {
    if (!consumer) return;

    bool inserted = false;
    {
        QMutexLocker locker(&m_directPreviewMutex);
        if (!m_directPreviewConsumers.contains(consumer)) {
            m_directPreviewConsumers.insert(consumer, DirectPreviewConsumerState{});
            inserted = true;
        }
    }
    if (!inserted) return;

    connect(consumer, &QObject::destroyed, this,
            [this, consumer]() { removeDirectPreviewConsumer(consumer); });
}

void FrameProvider::removeDirectPreviewConsumer(QObject* consumer) {
    if (!consumer) return;

    QMutexLocker locker(&m_directPreviewMutex);
    m_directPreviewConsumers.remove(consumer);
    m_directPreviewPainted.wakeAll();
}

void FrameProvider::markDirectPreviewConsumerPainted(QObject* consumer, quint64 serial) {
    if (!consumer || serial == 0) return;

    QMutexLocker locker(&m_directPreviewMutex);
    auto it = m_directPreviewConsumers.find(consumer);
    if (it == m_directPreviewConsumers.end()) return;
    it->paintedSerial = qMax(it->paintedSerial, serial);
    m_directPreviewPainted.wakeAll();
}

bool FrameProvider::flushDirectPreviewConsumers(int timeoutMs, quint64 minSerial) const {
    if (minSerial == 0) return true;

    const int boundedTimeoutMs = std::max(0, timeoutMs);
    QElapsedTimer timer;
    timer.start();

    auto allConsumersPainted = [this, minSerial]() {
        for (auto it = m_directPreviewConsumers.cbegin(); it != m_directPreviewConsumers.cend();
             ++it) {
            if (it.value().paintedSerial < minSerial) return false;
        }
        return true;
    };

    QMutexLocker locker(&m_directPreviewMutex);
    while (!allConsumersPainted()) {
        const qint64 elapsedMs = timer.elapsed();
        const int remainingMs = int(std::max<qint64>(0, qint64(boundedTimeoutMs) - elapsedMs));
        if (remainingMs <= 0) return false;
        if (!m_directPreviewPainted.wait(&m_directPreviewMutex,
                                         static_cast<unsigned long>(remainingMs))) {
            return allConsumersPainted();
        }
    }

    return true;
}

QImage FrameProvider::latestImage() const {
    return latestImage(nullptr);
}

QImage FrameProvider::latestImage(quint64* serial) const {
    QMutexLocker locker(&m_frameMutex);
    if (serial) *serial = m_frameSerial;
    if (!m_lastFrame.isValid()) return QImage();

    QVideoFrame frameCopy = m_lastFrame;
    QImage img = frameCopy.toImage();
    return img;
}

bool FrameProvider::latestFrameForSerial(quint64 serial, QVideoFrame* frame) const {
    if (!frame) return false;

    QMutexLocker locker(&m_frameMutex);
    if (serial != m_frameSerial || !m_lastFrame.isValid()) return false;

    *frame = m_lastFrame;
    return true;
}

qint64 FrameProvider::nextDisplayStartUsLocked() {
    const qint64 elapsedUs = m_displayTimer.isValid() ? (m_displayTimer.nsecsElapsed() / 1000) : 0;
    m_lastDisplayStartUs = qMax(elapsedUs, m_lastDisplayStartUs + 1);
    return m_lastDisplayStartUs;
}

void FrameProvider::markSinkAppliedSerial(QVideoSink* sink, quint64 serial) {
    if (!sink || serial == 0) return;
    QMutexLocker locker(&m_sinkMutex);
    m_appliedSerialBySink[sink] = qMax(m_appliedSerialBySink.value(sink, 0), serial);
}

quint64 FrameProvider::sinkAppliedSerial(QVideoSink* sink) const {
    if (!sink) return 0;
    QMutexLocker locker(&m_sinkMutex);
    return m_appliedSerialBySink.value(sink, 0);
}
