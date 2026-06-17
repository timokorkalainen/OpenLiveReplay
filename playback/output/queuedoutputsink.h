#ifndef QUEUEDOUTPUTSINK_H
#define QUEUEDOUTPUTSINK_H

#include "playback/output/outputsink.h"

#include <QMutex>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <memory>

class QueuedOutputSink final : public IOutputSink {
public:
    explicit QueuedOutputSink(std::unique_ptr<IOutputSink> inner, int capacity = 3);
    ~QueuedOutputSink() override;

    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override;
    bool submit(const OutputBusFrame& frame) override;
    OutputSinkStatus outputStatus() const override;

    int droppedFrames() const;

private:
    void workerLoop();

    std::unique_ptr<IOutputSink> m_inner;
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    int m_capacity = 3;

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    QVector<OutputBusFrame> m_queue;
    std::unique_ptr<QThread> m_thread;
    bool m_active = false;
    bool m_stopRequested = false;
    int m_droppedFrames = 0;
    qint64 m_asyncAcceptedFrames = 0;
    qint64 m_asyncFailedFrames = 0;
    qint64 m_maxQueueDepth = 0;
    qint64 m_deliveryGaps = 0;
    qint64 m_lastQueuedFrameIndex = -1;
    qint64 m_lastDeliveredFrameIndex = -1;
    bool m_queuePressure = false;
    bool m_lastSubmitDroppedFrame = false;
    bool m_lastDeliveryGap = false;
    bool m_hasLastAsyncResult = false;
    bool m_lastAsyncResultSucceeded = true;
    bool m_hasLastQueuedFrameIndex = false;
    bool m_hasLastDeliveredFrameIndex = false;
};

#endif // QUEUEDOUTPUTSINK_H
