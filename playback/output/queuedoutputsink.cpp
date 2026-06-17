#include "playback/output/queuedoutputsink.h"

#include <utility>

QueuedOutputSink::QueuedOutputSink(std::unique_ptr<IOutputSink> inner, int capacity)
    : m_inner(std::move(inner)), m_capacity(qMax(1, capacity)) {
    if (m_inner) m_kind = m_inner->kind();
}

QueuedOutputSink::~QueuedOutputSink() {
    stop();
}

bool QueuedOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    stop();
    if (!m_inner || !m_inner->start(assignment, rate)) return false;

    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
        m_stopRequested = false;
        m_active = true;
        m_droppedFrames = 0;
        m_asyncAcceptedFrames = 0;
        m_asyncFailedFrames = 0;
        m_hasLastAsyncResult = false;
        m_lastAsyncResultSucceeded = true;
    }

    m_thread.reset(QThread::create([this]() { workerLoop(); }));
    m_thread->start();
    return true;
}

void QueuedOutputSink::stop() {
    std::unique_ptr<QThread> thread;
    {
        QMutexLocker locker(&m_mutex);
        m_active = false;
        m_stopRequested = true;
        m_queue.clear();
        m_wake.wakeAll();
        thread = std::move(m_thread);
    }

    if (thread) thread->wait();
    if (m_inner) m_inner->stop();
}

bool QueuedOutputSink::isActive() const {
    QMutexLocker locker(&m_mutex);
    return m_active && !m_stopRequested;
}

bool QueuedOutputSink::submit(const OutputBusFrame& frame) {
    QMutexLocker locker(&m_mutex);
    if (!m_active || m_stopRequested || !m_thread) return false;
    if (m_queue.size() >= m_capacity) {
        m_queue.removeFirst();
        m_droppedFrames++;
    }
    m_queue.append(frame);
    m_wake.wakeOne();
    return true;
}

int QueuedOutputSink::droppedFrames() const {
    QMutexLocker locker(&m_mutex);
    return m_droppedFrames;
}

OutputSinkStatus QueuedOutputSink::outputStatus() const {
    OutputSinkStatus own;
    {
        QMutexLocker locker(&m_mutex);
        own.acceptedFrames = m_asyncAcceptedFrames;
        own.failedFrames = m_asyncFailedFrames;
        own.droppedFrames = m_droppedFrames;
        own.hasLastResult = m_hasLastAsyncResult;
        own.lastResultSucceeded = m_lastAsyncResultSucceeded;
    }

    const OutputSinkStatus inner = m_inner ? m_inner->outputStatus() : OutputSinkStatus{};
    own.acceptedFrames = qMax(own.acceptedFrames, inner.acceptedFrames);
    own.failedFrames = qMax(own.failedFrames, inner.failedFrames);
    own.droppedFrames += inner.droppedFrames;
    if (inner.hasLastResult) {
        own.hasLastResult = true;
        own.lastResultSucceeded = inner.lastResultSucceeded;
    }
    if (!inner.state.isEmpty()) own.state = inner.state;
    if (!inner.message.isEmpty()) own.message = inner.message;
    return own;
}

void QueuedOutputSink::workerLoop() {
    while (true) {
        OutputBusFrame frame;
        {
            QMutexLocker locker(&m_mutex);
            while (!m_stopRequested && m_queue.isEmpty()) {
                m_wake.wait(&m_mutex);
            }
            if (m_stopRequested) return;
            frame = m_queue.takeFirst();
        }
        const bool submitted = m_inner && m_inner->submit(frame);
        {
            QMutexLocker locker(&m_mutex);
            if (submitted) {
                m_asyncAcceptedFrames++;
            } else {
                m_asyncFailedFrames++;
            }
            m_hasLastAsyncResult = true;
            m_lastAsyncResultSucceeded = submitted;
        }
    }
}
