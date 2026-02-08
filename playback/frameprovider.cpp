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
    if (m_sink != sink) {
        if (m_sink) {
            disconnect(m_sink, nullptr, this, nullptr);
        }
        m_sink = sink;
        if (m_sink) {
            connect(m_sink, &QObject::destroyed, this, [this]() {
                m_sink = nullptr;
            });
        }
        emit videoSinkChanged();
        qDebug() << "C++: Video Sink successfully connected from QML";

        // Push the last available frame immediately for fast switching
        if (m_sink) {
            QVideoFrame lastFrameCopy;
            {
                QMutexLocker locker(&m_frameMutex);
                lastFrameCopy = m_lastFrame;
            }
            if (lastFrameCopy.isValid()) {
                if (m_sink->thread() == QThread::currentThread()) {
                    m_sink->setVideoFrame(lastFrameCopy);
                } else {
                    QPointer<QVideoSink> sinkPtr = m_sink;
                    QVideoFrame copy = lastFrameCopy;
                    QMetaObject::invokeMethod(m_sink, [sinkPtr, copy]() mutable {
                        if (sinkPtr) sinkPtr->setVideoFrame(copy);
                    }, Qt::QueuedConnection);
                }
            }
        }
    }
}

void FrameProvider::deliverFrame(const QVideoFrame &frame)
{
    {
        QMutexLocker locker(&m_frameMutex);
        m_lastFrame = frame;
    }

    QPointer<QVideoSink> sink = m_sink;
    if (!sink) return;

    if (sink->thread() == QThread::currentThread()) {
        sink->setVideoFrame(frame);
        return;
    }

    QVideoFrame copy = frame;
    QMetaObject::invokeMethod(sink, [sink, copy]() mutable {
        if (sink) sink->setVideoFrame(copy);
    }, Qt::QueuedConnection);
}

QImage FrameProvider::latestImage() const
{
    QMutexLocker locker(&m_frameMutex);
    if (!m_lastFrame.isValid()) return QImage();

    QVideoFrame frameCopy = m_lastFrame;
    QImage img = frameCopy.toImage();
    return img;
}
