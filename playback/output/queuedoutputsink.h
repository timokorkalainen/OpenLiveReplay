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
};

#endif // QUEUEDOUTPUTSINK_H
