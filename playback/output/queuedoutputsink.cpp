#include "playback/output/queuedoutputsink.h"

#include <QElapsedTimer>

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
        m_delivering = false;
        ++m_epoch;
        m_droppedFrames = 0;
        m_droppedFrameIndexes.clear();
        m_asyncAcceptedFrames = 0;
        m_asyncFailedFrames = 0;
        m_maxQueueDepth = 0;
        m_deliveryGaps = 0;
        m_lastQueuedFrameIndex = -1;
        m_lastDeliveredFrameIndex = -1;
        m_queuePressure = false;
        m_lastSubmitDroppedFrame = false;
        m_lastDeliveryGap = false;
        m_hasLastAsyncResult = false;
        m_lastAsyncResultSucceeded = true;
        m_hasLastQueuedFrameIndex = false;
        m_hasLastDeliveredFrameIndex = false;
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
        m_drained.wakeAll();
        thread = std::move(m_thread);
    }

    if (thread) thread->wait();
    {
        QMutexLocker locker(&m_mutex);
        while (m_delivering) {
            m_drained.wait(&m_mutex);
        }
    }
    if (m_inner) m_inner->stop();
}

bool QueuedOutputSink::isActive() const {
    QMutexLocker locker(&m_mutex);
    return m_active && !m_stopRequested;
}

bool QueuedOutputSink::submit(const OutputBusFrame& frame) {
    QMutexLocker locker(&m_mutex);
    if (!m_active || m_stopRequested || !m_thread) return false;
    bool dropped = false;
    if (m_queue.size() >= m_capacity) {
        m_droppedFrameIndexes.append(m_queue.first().frame.outputFrameIndex);
        // Bound the tracked-drop history. Indexes are normally drained by the next
        // successful delivery; only a persistently-failing inner sink (already reported as
        // Error via the rejection path) lets them accumulate. Forgetting the oldest drop can
        // only make a future gap look unexplained, which errs toward Error — the correct
        // outcome in that all-reject state — so it never masks a real failure.
        constexpr int kMaxTrackedDrops = 4096;
        if (m_droppedFrameIndexes.size() > kMaxTrackedDrops) m_droppedFrameIndexes.removeFirst();
        m_queue.removeFirst();
        m_droppedFrames++;
        dropped = true;
    }
    m_queue.append(QueuedFrame{frame, m_epoch});
    m_lastQueuedFrameIndex = frame.outputFrameIndex;
    m_hasLastQueuedFrameIndex = true;
    m_maxQueueDepth = qMax<qint64>(m_maxQueueDepth, m_queue.size());
    m_lastSubmitDroppedFrame = dropped;
    m_queuePressure = m_queue.size() > 1;
    m_wake.wakeOne();
    return true;
}

bool QueuedOutputSink::submitAndFlush(const OutputBusFrame& frame, int timeoutMs) {
    quint64 epoch = 0;
    bool useQueuedPath = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_active || m_stopRequested || !m_thread) return false;
        useQueuedPath = !m_queue.isEmpty() || m_delivering;
        if (!useQueuedPath) {
            epoch = m_epoch;
            m_lastQueuedFrameIndex = frame.outputFrameIndex;
            m_hasLastQueuedFrameIndex = true;
            m_lastSubmitDroppedFrame = false;
            m_queuePressure = false;
            m_delivering = true;
        } else {
            const qsizetype droppedQueuedFrames = m_queue.size();
            for (const QueuedFrame& queued : std::as_const(m_queue))
                m_droppedFrameIndexes.append(queued.frame.outputFrameIndex);
            m_queue.clear();
            if (droppedQueuedFrames > 0) {
                m_droppedFrames += static_cast<int>(droppedQueuedFrames);
                constexpr int kMaxTrackedDrops = 4096;
                while (m_droppedFrameIndexes.size() > kMaxTrackedDrops)
                    m_droppedFrameIndexes.removeFirst();
            }
            ++m_epoch;
            m_queue.append(QueuedFrame{frame, m_epoch});
            m_lastQueuedFrameIndex = frame.outputFrameIndex;
            m_hasLastQueuedFrameIndex = true;
            m_maxQueueDepth = qMax<qint64>(m_maxQueueDepth, m_queue.size());
            m_lastSubmitDroppedFrame = droppedQueuedFrames > 0;
            m_queuePressure = false;
            m_wake.wakeOne();
        }
    }

    if (useQueuedPath) return flush(timeoutMs);

    const bool submitted = m_inner && m_inner->submitAndFlush(frame, timeoutMs);
    {
        QMutexLocker locker(&m_mutex);
        recordDeliveryResultLocked(frame, epoch, submitted);
        m_delivering = false;
        if (!m_queue.isEmpty()) m_wake.wakeOne();
        m_drained.wakeAll();
    }
    return submitted;
}

int QueuedOutputSink::droppedFrames() const {
    QMutexLocker locker(&m_mutex);
    return m_droppedFrames;
}

bool QueuedOutputSink::flush(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    QMutexLocker locker(&m_mutex);
    while (!m_stopRequested && (!m_queue.isEmpty() || m_delivering)) {
        const qint64 remainingMs = qint64(timeoutMs) - timer.elapsed();
        if (remainingMs <= 0) return false;
        m_drained.wait(&m_mutex, static_cast<unsigned long>(remainingMs));
    }
    return !m_stopRequested;
}

void QueuedOutputSink::discardPending() {
    {
        QMutexLocker locker(&m_mutex);
        m_queue.clear();
        m_droppedFrameIndexes.clear();
        m_queuePressure = false;
        m_lastSubmitDroppedFrame = false;
        m_lastDeliveryGap = false;
        m_lastQueuedFrameIndex = -1;
        m_lastDeliveredFrameIndex = -1;
        m_hasLastQueuedFrameIndex = false;
        m_hasLastDeliveredFrameIndex = false;
        ++m_epoch;
        m_wake.wakeAll();
        m_drained.wakeAll();
    }
    if (m_inner) m_inner->discardPending();
}

OutputSinkStatus QueuedOutputSink::outputStatus() const {
    OutputSinkStatus own;
    {
        QMutexLocker locker(&m_mutex);
        own.acceptedFrames = m_asyncAcceptedFrames;
        own.failedFrames = m_asyncFailedFrames;
        own.droppedFrames = m_droppedFrames;
        own.currentQueueDepth = m_queue.size();
        own.maxQueueDepth = m_maxQueueDepth;
        own.deliveryGaps = m_deliveryGaps;
        own.lastQueuedFrameIndex = m_lastQueuedFrameIndex;
        own.lastDeliveredFrameIndex = m_lastDeliveredFrameIndex;
        own.queuePressure = m_queuePressure;
        own.lastSubmitDroppedFrame = m_lastSubmitDroppedFrame;
        own.lastDeliveryGap = m_lastDeliveryGap;
        own.hasLastResult = m_hasLastAsyncResult;
        own.lastResultSucceeded = m_lastAsyncResultSucceeded;
        own.hasLastQueuedFrameIndex = m_hasLastQueuedFrameIndex;
        own.hasLastDeliveredFrameIndex = m_hasLastDeliveredFrameIndex;
    }

    const OutputSinkStatus inner = m_inner ? m_inner->outputStatus() : OutputSinkStatus{};
    own.acceptedFrames = qMax(own.acceptedFrames, inner.acceptedFrames);
    own.failedFrames = qMax(own.failedFrames, inner.failedFrames);
    own.droppedFrames += inner.droppedFrames;
    own.currentQueueDepth += inner.currentQueueDepth;
    own.maxQueueDepth = qMax(own.maxQueueDepth, inner.maxQueueDepth);
    own.deliveryGaps += inner.deliveryGaps;
    own.queuePressure = own.queuePressure || inner.queuePressure;
    own.lastSubmitDroppedFrame = own.lastSubmitDroppedFrame || inner.lastSubmitDroppedFrame;
    own.lastDeliveryGap = own.lastDeliveryGap || inner.lastDeliveryGap;
    if (inner.hasLastQueuedFrameIndex) {
        own.hasLastQueuedFrameIndex = true;
        own.lastQueuedFrameIndex = inner.lastQueuedFrameIndex;
    }
    if (inner.hasLastDeliveredFrameIndex) {
        own.hasLastDeliveredFrameIndex = true;
        own.lastDeliveredFrameIndex = inner.lastDeliveredFrameIndex;
    }
    own.lastSubmitDurationNs = qMax(own.lastSubmitDurationNs, inner.lastSubmitDurationNs);
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
        QueuedFrame queued;
        {
            QMutexLocker locker(&m_mutex);
            while (!m_stopRequested && (m_queue.isEmpty() || m_delivering)) {
                m_wake.wait(&m_mutex);
            }
            if (m_stopRequested) return;
            queued = m_queue.takeFirst();
            m_queuePressure = m_queue.size() > 1;
            m_delivering = true;
        }
        const OutputBusFrame frame = queued.frame;
        const bool submitted = m_inner && m_inner->submit(frame);
        {
            QMutexLocker locker(&m_mutex);
            recordDeliveryResultLocked(frame, queued.epoch, submitted);
            m_delivering = false;
            if (!m_queue.isEmpty()) m_wake.wakeOne();
            m_drained.wakeAll();
        }
    }
}

void QueuedOutputSink::recordDeliveryResultLocked(const OutputBusFrame& frame, quint64 epoch,
                                                  bool submitted) {
    const bool currentEpoch = epoch == m_epoch;
    if (submitted && currentEpoch) {
        // Consume the overflow-dropped indexes that precede this delivered frame.
        // For a real gap (non-first delivery) these are exactly the missing indexes
        // between the previous and current delivered frame that were dropped. Drops
        // below the first-ever delivered index bracket no computed gap and are simply
        // discarded here, scoped by index so they can never carry forward to suppress
        // a later, independently-caused gap.
        int droppedInGap = 0;
        while (!m_droppedFrameIndexes.isEmpty() &&
               m_droppedFrameIndexes.first() < frame.outputFrameIndex) {
            m_droppedFrameIndexes.removeFirst();
            droppedInGap++;
        }
        if (m_hasLastDeliveredFrameIndex) {
            const qint64 gapSize =
                qMax<qint64>(0, frame.outputFrameIndex - m_lastDeliveredFrameIndex - 1);
            if (gapSize > 0) {
                m_deliveryGaps++;
            }
            // A gap fully explained by queue-overflow drops is backpressure
            // (surfaced as Degraded via lastSubmitDroppedFrame), not a delivery
            // failure. Only raise the Error-mapping lastDeliveryGap when missing
            // indexes remain unexplained by drops (e.g. an inner-sink rejection).
            m_lastDeliveryGap = gapSize > 0 && droppedInGap < gapSize;
        }
        m_lastDeliveredFrameIndex = frame.outputFrameIndex;
        m_hasLastDeliveredFrameIndex = true;
        m_asyncAcceptedFrames++;
    } else if (!submitted && currentEpoch) {
        m_asyncFailedFrames++;
    }
    if (currentEpoch) {
        m_hasLastAsyncResult = true;
        m_lastAsyncResultSucceeded = submitted;
    }
}
