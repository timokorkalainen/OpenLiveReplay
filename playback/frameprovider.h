#ifndef FRAMEPROVIDER_H
#define FRAMEPROVIDER_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QMutex>

class FrameProvider : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)

public:
    explicit FrameProvider(QObject *parent = nullptr);

    QVideoSink* videoSink() const;
    void setVideoSink(QVideoSink *sink);

    // This method is called by the PlaybackWorker to push new frames to the UI
    void deliverFrame(const QVideoFrame &frame);

    // Retrieve the latest frame as an image (for screenshots)
    QImage latestImage() const;

signals:
    void videoSinkChanged();

private:
    QVideoSink *m_sink = nullptr;
    mutable QMutex m_frameMutex;
    QVideoFrame m_lastFrame;
};

#endif // FRAMEPROVIDER_H
