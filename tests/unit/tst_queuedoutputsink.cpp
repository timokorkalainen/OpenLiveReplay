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

class TestQueuedOutputSink : public QObject {
    Q_OBJECT
private slots:
    void submitReturnsBeforeSlowInnerSinkCompletes();
    void asyncInnerSubmitFailuresAreVisibleInStatus();
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

QTEST_GUILESS_MAIN(TestQueuedOutputSink)
#include "tst_queuedoutputsink.moc"
