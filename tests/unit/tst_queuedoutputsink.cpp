#include <QtTest>

#include "playback/output/queuedoutputsink.h"

static OutputBusFrame frame(qint64 index) {
    OutputBusFrame out;
    out.bus = OutputBusId::feed(0);
    out.outputFrameIndex = index;
    out.video = MediaVideoFrame::solidYuv420p(4, 4, uchar(40 + index), 128, 128);
    out.video.feedIndex = 0;
    out.video.ptsMs = index * 40;
    out.audio.feedIndex = 0;
    out.audio.sampleRate = 48000;
    out.audio.channels = 2;
    out.audio.format = MediaSampleFormat::S16Interleaved;
    out.audio.pcm = QByteArray(160 * 2 * int(sizeof(qint16)), '\0');
    return out;
}

class SlowCollectingSink final : public IOutputSink {
public:
    explicit SlowCollectingSink(bool failSubmits = false) : m_failSubmits(failSubmits) {}

    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        QMutexLocker locker(&m_mutex);
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }

    void stop() override {
        QMutexLocker locker(&m_mutex);
        m_active = false;
    }

    bool isActive() const override {
        QMutexLocker locker(&m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame& frame) override {
        QThread::msleep(120);
        QMutexLocker locker(&m_mutex);
        if (!m_active) return false;
        m_submitAttempts++;
        if (m_failSubmits) return false;
        m_frames.append(frame);
        return true;
    }

    int frameCount() const {
        QMutexLocker locker(&m_mutex);
        return m_frames.size();
    }

    QVector<OutputBusFrame> frames() const {
        QMutexLocker locker(&m_mutex);
        return m_frames;
    }

    int submitAttempts() const {
        QMutexLocker locker(&m_mutex);
        return m_submitAttempts;
    }

private:
    mutable QMutex m_mutex;
    bool m_active = false;
    bool m_failSubmits = false;
    int m_submitAttempts = 0;
    QVector<OutputBusFrame> m_frames;
};

class BlockingInnerSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        QMutexLocker locker(&m_mutex);
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        m_released = false;
        m_enteredSubmits = 0;
        m_frames.clear();
        return m_active;
    }

    void stop() override {
        QMutexLocker locker(&m_mutex);
        m_active = false;
        m_released = true;
        m_releasedChanged.wakeAll();
    }

    bool isActive() const override {
        QMutexLocker locker(&m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame& frame) override {
        QMutexLocker locker(&m_mutex);
        if (!m_active) return false;

        m_enteredSubmits++;
        m_enteredChanged.wakeAll();
        while (m_active && !m_released) {
            m_releasedChanged.wait(&m_mutex);
        }
        if (!m_active) return false;

        m_frames.append(frame);
        return true;
    }

    bool waitForEnteredSubmits(int count, int timeoutMs) const {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&m_mutex);
        while (m_enteredSubmits < count) {
            const qint64 remainingMs = timeoutMs - timer.elapsed();
            if (remainingMs <= 0) return false;
            m_enteredChanged.wait(&m_mutex, static_cast<unsigned long>(remainingMs));
        }
        return true;
    }

    void release() {
        QMutexLocker locker(&m_mutex);
        m_released = true;
        m_releasedChanged.wakeAll();
    }

private:
    mutable QMutex m_mutex;
    mutable QWaitCondition m_enteredChanged;
    QWaitCondition m_releasedChanged;
    bool m_active = false;
    bool m_released = false;
    int m_enteredSubmits = 0;
    QVector<OutputBusFrame> m_frames;
};

class GapReportingInnerSink final : public IOutputSink {
public:
    explicit GapReportingInnerSink(qint64 rejectedIndex = -1) : m_rejectedIndex(rejectedIndex) {}

    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        QMutexLocker locker(&m_mutex);
        m_active = assignment.enabled && assignment.kind == kind() && rate.isValid();
        return m_active;
    }

    void stop() override {
        QMutexLocker locker(&m_mutex);
        m_active = false;
    }

    bool isActive() const override {
        QMutexLocker locker(&m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame& frame) override {
        QMutexLocker locker(&m_mutex);
        m_attempted.append(frame.outputFrameIndex);
        if (!m_active || frame.outputFrameIndex == m_rejectedIndex) return false;
        m_delivered.append(frame.outputFrameIndex);
        return true;
    }

    QVector<qint64> delivered() const {
        QMutexLocker locker(&m_mutex);
        return m_delivered;
    }

private:
    mutable QMutex m_mutex;
    bool m_active = false;
    qint64 m_rejectedIndex = -1;
    QVector<qint64> m_attempted;
    QVector<qint64> m_delivered;
};

class TestQueuedOutputSink : public QObject {
    Q_OBJECT
private slots:
    void submitReturnsBeforeSlowInnerSinkCompletes();
    void asyncInnerSubmitFailuresAreVisibleInStatus();
    void queueStatusReportsDepthAndDroppedFrames();
    void deliveryGapsAreVisibleInStatus();
    void failedInnerSubmitDoesNotAdvanceDeliveredFrameIndex();
};

void TestQueuedOutputSink::submitReturnsBeforeSlowInnerSinkCompletes() {
    auto inner = std::make_unique<SlowCollectingSink>();
    SlowCollectingSink* observed = inner.get();
    QueuedOutputSink sink(std::move(inner), 3);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));

    QElapsedTimer timer;
    timer.start();
    QVERIFY(sink.submit(frame(0)));
    QVERIFY2(timer.elapsed() < 40, "queued submit must not block on the slow inner sink");

    QTRY_COMPARE_WITH_TIMEOUT(observed->frameCount(), 1, 500);
    const QVector<OutputBusFrame> frames = observed->frames();
    QCOMPARE(frames[0].outputFrameIndex, qint64(0));

    sink.stop();
}

void TestQueuedOutputSink::asyncInnerSubmitFailuresAreVisibleInStatus() {
    auto inner = std::make_unique<SlowCollectingSink>(true);
    SlowCollectingSink* observed = inner.get();
    QueuedOutputSink sink(std::move(inner), 3);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(4)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->submitAttempts(), 1, 500);
    QTRY_COMPARE_WITH_TIMEOUT(sink.outputStatus().failedFrames, qint64(1), 500);
    QVERIFY(sink.outputStatus().hasLastResult);
    QVERIFY(!sink.outputStatus().lastResultSucceeded);

    sink.stop();
}

void TestQueuedOutputSink::queueStatusReportsDepthAndDroppedFrames() {
    auto inner = std::make_unique<BlockingInnerSink>();
    BlockingInnerSink* observed = inner.get();
    QueuedOutputSink sink(std::move(inner), 1);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(10)));
    const bool workerBlocked = observed->waitForEnteredSubmits(1, 500);
    if (!workerBlocked) {
        observed->release();
        sink.stop();
    }
    QVERIFY2(workerBlocked,
             "worker must enter the blocking inner sink before queue pressure is asserted");

    const bool submitted11 = sink.submit(frame(11));
    const bool submitted12 = sink.submit(frame(12));

    const OutputSinkStatus status = sink.outputStatus();
    observed->release();
    sink.stop();

    QVERIFY(submitted11);
    QVERIFY(submitted12);
    QVERIFY(status.maxQueueDepth >= 1);
    QVERIFY(status.droppedFrames >= 1);
    QCOMPARE(status.lastQueuedFrameIndex, qint64(12));
}

void TestQueuedOutputSink::deliveryGapsAreVisibleInStatus() {
    auto inner = std::make_unique<GapReportingInnerSink>();
    GapReportingInnerSink* observed = inner.get();
    QueuedOutputSink sink(std::move(inner), 3);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(20)));
    QVERIFY(sink.submit(frame(22)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->delivered().size(), 2, 500);
    const OutputSinkStatus status = sink.outputStatus();
    QVERIFY(status.deliveryGaps > 0);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(22));

    sink.stop();
}

void TestQueuedOutputSink::failedInnerSubmitDoesNotAdvanceDeliveredFrameIndex() {
    auto inner = std::make_unique<GapReportingInnerSink>(21);
    GapReportingInnerSink* observed = inner.get();
    QueuedOutputSink sink(std::move(inner), 3);

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(sink.submit(frame(20)));
    QVERIFY(sink.submit(frame(21)));
    QVERIFY(sink.submit(frame(22)));

    QTRY_COMPARE_WITH_TIMEOUT(observed->delivered(), (QVector<qint64>{20, 22}), 500);
    const OutputSinkStatus status = sink.outputStatus();
    QVERIFY(status.deliveryGaps > 0);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(22));

    sink.stop();
}

QTEST_GUILESS_MAIN(TestQueuedOutputSink)
#include "tst_queuedoutputsink.moc"
