#include "frameprovider.h"
#include <QtCore/qdebug.h>

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
    if (m_sink) {
        m_sink->setVideoFrame(frame);
    }
}
