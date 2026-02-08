#include "frameprovider.h"
#include <QtCore/qdebug.h>
#include <QMetaObject>
#include <QThread>

FrameProvider::FrameProvider(QObject *parent)
    : QObject(parent)
{
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
    {
        QMutexLocker locker(&m_frameMutex);
        lastFrameCopy = m_lastFrame;
    }
    if (lastFrameCopy.isValid()) {
        if (sink->thread() == QThread::currentThread()) {
            sink->setVideoFrame(lastFrameCopy);
        } else {
            QPointer<QVideoSink> sinkPtr = sink;
            QVideoFrame copy = lastFrameCopy;
            QMetaObject::invokeMethod(sink, [sinkPtr, copy]() mutable {
                if (sinkPtr) sinkPtr->setVideoFrame(copy);
            }, Qt::QueuedConnection);
        }
    }
}

void FrameProvider::removeVideoSink(QVideoSink *sink)
{
    if (!sink) return;

    QMutexLocker locker(&m_sinkMutex);
    for (int i = m_sinks.size() - 1; i >= 0; --i) {
        if (!m_sinks[i] || m_sinks[i] == sink) {
            m_sinks.removeAt(i);
        }
    }

    if (m_sink == sink) {
        m_sink = nullptr;
    }
}

void FrameProvider::deliverFrame(const QVideoFrame &frame)
{
    {
        QMutexLocker locker(&m_frameMutex);
        m_lastFrame = frame;
    }

    QList<QPointer<QVideoSink>> sinksCopy;
    {
        QMutexLocker locker(&m_sinkMutex);
        sinksCopy = m_sinks;
    }

    if (sinksCopy.isEmpty()) return;

    for (const auto &sink : sinksCopy) {
        if (!sink) continue;
        if (sink->thread() == QThread::currentThread()) {
            sink->setVideoFrame(frame);
        } else {
            QVideoFrame copy = frame;
            QPointer<QVideoSink> sinkPtr = sink;
            QMetaObject::invokeMethod(sink, [sinkPtr, copy]() mutable {
                if (sinkPtr) sinkPtr->setVideoFrame(copy);
            }, Qt::QueuedConnection);
        }
    }
}

QImage FrameProvider::latestImage() const
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_lastFrame.isValid()) return QImage();

    QVideoFrame frameCopy = m_lastFrame;
    QImage img = frameCopy.toImage();
    return img;
}
