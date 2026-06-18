#include <QtTest>

#include "playback/output/outputdispatcher.h"

static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = pts;
    return f;
}

class CollectingSink final : public IOutputSink {
public:
    explicit CollectingSink(OutputTargetKind kind) : m_kind(kind) {}
    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override {
        if (!m_active) return false;
        frames.append(frame);
        return true;
    }
    QVector<OutputBusFrame> frames;

private:
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    bool m_active = false;
};

class TestOutputDispatcherHoldLast : public QObject {
    Q_OBJECT
private slots:
    void emptyCacheAfterRealFrameHoldsLastGoodVideo();
    void holdDisabledLeavesPlaceholderVisible();
};

void TestOutputDispatcherHoldLast::emptyCacheAfterRealFrameHoldsLastGoodVideo() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setHoldLastFrame(true); // default, asserted explicitly
    dispatcher.setEndpoints({{feed0, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    // Tick 1: a real frame is present.
    OutputFrameCache full(1, 4, 4);
    full.insertVideoFrame(video(0, 100, 77));
    dispatcher.dispatchTick(full, state);

    // Tick 2: the cache went empty (mid-seek) → renderBus yields a placeholder.
    OutputFrameCache empty(1, 4, 4);
    dispatcher.dispatchTick(empty, state);

    QCOMPARE(sink.frames.size(), 2);
    // The held tick must NOT be the gray placeholder...
    QVERIFY(!sink.frames[1].video.isPlaceholder);
    // ...and its video pixels must equal the prior real frame.
    QCOMPARE(uchar(sink.frames[1].video.planeY.at(0)), uchar(77));
    QCOMPARE(sink.frames[1].video.planeY, sink.frames[0].video.planeY);
    // Held frame keeps the fresh tick's outputFrameIndex (clock never stalls).
    QCOMPARE(sink.frames[1].outputFrameIndex, qint64(1));

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.heldFrames, qint64(1));
    QCOMPARE(stats.placeholderFrames, qint64(0));
}

void TestOutputDispatcherHoldLast::holdDisabledLeavesPlaceholderVisible() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setHoldLastFrame(false);
    dispatcher.setEndpoints({{feed0, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputFrameCache full(1, 4, 4);
    full.insertVideoFrame(video(0, 100, 77));
    dispatcher.dispatchTick(full, state);

    OutputFrameCache empty(1, 4, 4);
    dispatcher.dispatchTick(empty, state);

    QCOMPARE(sink.frames.size(), 2);
    QVERIFY(sink.frames[1].video.isPlaceholder);
    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.heldFrames, qint64(0));
    QCOMPARE(stats.placeholderFrames, qint64(1));
}

QTEST_GUILESS_MAIN(TestOutputDispatcherHoldLast)
#include "tst_outputdispatcher_holdlast.moc"
