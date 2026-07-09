#ifndef FRAMEPROVIDER_H
#define FRAMEPROVIDER_H

#include <QObject>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QHash>
#include <QElapsedTimer>
#include <QMutex>
#include <QPointer>
#include <QList>
#include <QSet>
#include <QWaitCondition>

class FrameHandle;

class FrameProvider : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)

public:
    explicit FrameProvider(QObject *parent = nullptr);

    QVideoSink* videoSink() const;
    void setVideoSink(QVideoSink *sink);

    Q_INVOKABLE void addVideoSink(QVideoSink *sink);
    Q_INVOKABLE void removeVideoSink(QVideoSink *sink);

    // This method is called by the PlaybackWorker to push new frames to the UI
    quint64 deliverFrame(const QVideoFrame& frame);
    quint64 deliverHandle(const FrameHandle& handle);
    bool flushVideoSinks(int timeoutMs, quint64 minSerial = 0) const;
    Q_INVOKABLE void addDirectPreviewConsumer(QObject* consumer);
    Q_INVOKABLE void removeDirectPreviewConsumer(QObject* consumer);
    void markDirectPreviewConsumerPainted(QObject* consumer, quint64 serial);
    bool flushDirectPreviewConsumers(int timeoutMs, quint64 minSerial = 0) const;
    bool hasPreviewConsumers() const;

    // Retrieve the latest frame as an image (for screenshots)
    QImage latestImage() const;
    QImage latestImage(quint64* serial) const;

signals:
    void videoSinkChanged();
    void frameChanged(quint64 serial);

private:
    void queueLatestFrameForSink(QVideoSink* sink);
    bool postLatestFrameForSink(QVideoSink* sink);
    void applyLatestFrameToSink(QVideoSink* sink);
    qint64 nextDisplayStartUsLocked();
    void markSinkAppliedSerial(QVideoSink* sink, quint64 serial);
    quint64 sinkAppliedSerial(QVideoSink* sink) const;

    struct DirectPreviewConsumerState {
        quint64 paintedSerial = 0;
    };

    QPointer<QVideoSink> m_sink;
    QList<QPointer<QVideoSink>> m_sinks;
    QSet<QVideoSink*> m_pendingSinkUpdates;
    QHash<QVideoSink*, quint64> m_appliedSerialBySink;
    QHash<QObject*, DirectPreviewConsumerState> m_directPreviewConsumers;
    mutable QMutex m_sinkMutex;
    mutable QMutex m_directPreviewMutex;
    mutable QWaitCondition m_directPreviewPainted;
    mutable QMutex m_frameMutex;
    QVideoFrame m_lastFrame;
    quint64 m_frameSerial = 0;
    QElapsedTimer m_displayTimer;
    qint64 m_lastDisplayStartUs = -1;
};

#endif // FRAMEPROVIDER_H
