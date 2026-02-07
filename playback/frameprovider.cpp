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
        m_sink = sink;
        emit videoSinkChanged();
        qDebug() << "C++: Video Sink successfully connected from QML";
    }
}

void FrameProvider::deliverFrame(const QVideoFrame &frame)
{
    {
        QMutexLocker locker(&m_frameMutex);
        m_lastFrame = frame;
    }

    if (!m_sink) return;

    if (m_sink->thread() == QThread::currentThread()) {
        m_sink->setVideoFrame(frame);
        return;
    }

    QVideoFrame copy = frame;
    QMetaObject::invokeMethod(m_sink, [sink = m_sink, copy]() mutable {
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
