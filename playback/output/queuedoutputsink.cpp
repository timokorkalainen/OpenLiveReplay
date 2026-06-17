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
        if (m_inner) m_inner->submit(frame);
    }
}
