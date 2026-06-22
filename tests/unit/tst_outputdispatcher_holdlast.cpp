#include <QtTest>

#include "playback/output/outputdispatcher.h"

static FrameHandle video(int feed, qint64 pts, uchar y) {
    FrameHandle f = solidYuv420pHandle(4, 4, y, 128, 128);
    f.metadata().key.feedIndex = feed;
    f.metadata().key.ptsMs = pts;
    return f;
}

static QByteArray yPlane(const OutputBusFrame& frame) {
    return MediaVideoFrameView(frame.video).planeY;
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
    void generationChangeDoesNotHoldStaleGpuFrame();
    void placeholderAfterHeldGpuFrameIsSubmittedWhenGenerationChanges();
    void samePtsNewGpuGenerationBypassesIdentitySkipForFeedAndPgm();
    void multiviewGenerationChangeDoesNotHoldStaleComposite();
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
    QVERIFY(!sink.frames[1].video.metadata().key.isPlaceholder);
    // ...and its video pixels must equal the prior real frame.
    QCOMPARE(uchar(yPlane(sink.frames[1]).at(0)), uchar(77));
    QCOMPARE(yPlane(sink.frames[1]), yPlane(sink.frames[0]));
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
    QVERIFY(sink.frames[1].video.metadata().key.isPlaceholder);
    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.heldFrames, qint64(0));
    QCOMPARE(stats.placeholderFrames, qint64(1));
}

void TestOutputDispatcherHoldLast::generationChangeDoesNotHoldStaleGpuFrame() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setHoldLastFrame(true);
    dispatcher.setEndpoints({{feed0, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;
    state.gpuGeneration = 1;

    OutputFrameCache full(1, 4, 4);
    FrameHandle first = video(0, 100, 77);
    first.metadata().gpuGeneration = 1;
    full.insertVideoFrame(first);
    dispatcher.dispatchTick(full, state);

    PlaybackStateSnapshot afterSeek = state;
    afterSeek.gpuGeneration = 2;
    OutputFrameCache empty(1, 4, 4);
    dispatcher.dispatchTick(empty, afterSeek);

    QCOMPARE(sink.frames.size(), 2);
    QVERIFY(sink.frames[1].video.metadata().key.isPlaceholder);
    QCOMPARE(uchar(yPlane(sink.frames[1]).at(0)), uchar(16));

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.heldFrames, qint64(0));
    QCOMPARE(stats.placeholderFrames, qint64(1));
}

void TestOutputDispatcherHoldLast::placeholderAfterHeldGpuFrameIsSubmittedWhenGenerationChanges() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setHoldLastFrame(true);
    dispatcher.setEndpoints({{feed0, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;
    state.gpuGeneration = 1;

    OutputFrameCache full(1, 4, 4);
    FrameHandle first = video(0, 100, 77);
    first.metadata().gpuGeneration = 1;
    full.insertVideoFrame(first);
    dispatcher.dispatchTick(full, state);

    OutputFrameCache empty(1, 4, 4);
    dispatcher.dispatchTick(empty, state);

    PlaybackStateSnapshot afterBump = state;
    afterBump.gpuGeneration = 2;
    dispatcher.dispatchTick(empty, afterBump);

    QCOMPARE(sink.frames.size(), 3);
    QVERIFY(!sink.frames[1].video.metadata().key.isPlaceholder);
    QCOMPARE(uchar(yPlane(sink.frames[1]).at(0)), uchar(77));
    QVERIFY(sink.frames[2].video.metadata().key.isPlaceholder);
    QCOMPARE(uchar(yPlane(sink.frames[2]).at(0)), uchar(16));

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.heldFrames, qint64(1));
    QCOMPARE(stats.placeholderFrames, qint64(1));
}

void TestOutputDispatcherHoldLast::samePtsNewGpuGenerationBypassesIdentitySkipForFeedAndPgm() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-preview");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::QtPreview;
    pgm.enabled = true;

    CollectingSink feedSink(OutputTargetKind::QtPreview);
    CollectingSink pgmSink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{feed0, &feedSink}, {pgm, &pgmSink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;
    state.gpuGeneration = 1;

    OutputFrameCache firstCache(1, 4, 4);
    FrameHandle first = video(0, 100, 77);
    first.metadata().gpuGeneration = 1;
    firstCache.insertVideoFrame(first);
    dispatcher.dispatchTick(firstCache, state);

    PlaybackStateSnapshot secondState = state;
    secondState.gpuGeneration = 2;
    OutputFrameCache secondCache(1, 4, 4);
    FrameHandle second = video(0, 100, 88);
    second.metadata().gpuGeneration = 2;
    secondCache.insertVideoFrame(second);
    dispatcher.dispatchTick(secondCache, secondState);

    QCOMPARE(feedSink.frames.size(), 2);
    QCOMPARE(pgmSink.frames.size(), 2);
    QCOMPARE(uchar(yPlane(feedSink.frames[1]).at(0)), uchar(88));
    QCOMPARE(uchar(yPlane(pgmSink.frames[1]).at(0)), uchar(88));
}

void TestOutputDispatcherHoldLast::multiviewGenerationChangeDoesNotHoldStaleComposite() {
    OutputTargetAssignment multiview;
    multiview.id = QStringLiteral("multiview-preview");
    multiview.sourceBus = OutputBusId::multiview();
    multiview.kind = OutputTargetKind::QtPreview;
    multiview.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setHoldLastFrame(true);
    dispatcher.setEndpoints({{multiview, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;
    state.gpuGeneration = 1;

    OutputFrameCache full(1, 4, 4);
    FrameHandle first = video(0, 100, 77);
    first.metadata().gpuGeneration = 1;
    full.insertVideoFrame(first);
    dispatcher.dispatchTick(full, state);

    PlaybackStateSnapshot afterSeek = state;
    afterSeek.gpuGeneration = 2;
    OutputFrameCache empty(1, 4, 4);
    dispatcher.dispatchTick(empty, afterSeek);

    QCOMPARE(sink.frames.size(), 2);
    QVERIFY(sink.frames[1].video.metadata().key.isPlaceholder);
    QCOMPARE(uchar(yPlane(sink.frames[1]).at(0)), uchar(16));

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.heldFrames, qint64(0));
    QCOMPARE(stats.placeholderFrames, qint64(1));
}

QTEST_GUILESS_MAIN(TestOutputDispatcherHoldLast)
#include "tst_outputdispatcher_holdlast.moc"
