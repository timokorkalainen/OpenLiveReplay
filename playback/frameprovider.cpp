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
