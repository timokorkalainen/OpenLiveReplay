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
        m_assignment = assignment;
        m_rate = rate;
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
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
};

class TestOutputDispatcher : public QObject {
    Q_OBJECT
private slots:
    void pausedTicksRepeatFramesContinuouslyForEverySink();
    void playingTicksCreateStableOutputPlayEpoch();
    void resetPlayEpochKeepsOutputFrameIndexContinuous();
    void rendersFeedMultiviewAndPgmAssignmentsFromSameTick();
    void disabledAssignmentsDoNotSubmit();
};

void TestOutputDispatcher::pausedTicksRepeatFramesContinuouslyForEverySink() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 40));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    OutputTargetAssignment ndi;
    ndi.id = QStringLiteral("feed0-ndi");
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;

    CollectingSink qtSink(OutputTargetKind::QtPreview);
    CollectingSink ndiSink(OutputTargetKind::Ndi);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{qt, &qtSink}, {ndi, &ndiSink}});

    dispatcher.dispatchTick(cache, state);
    dispatcher.dispatchTick(cache, state);

    QCOMPARE(qtSink.frames.size(), 2);
    QCOMPARE(ndiSink.frames.size(), 2);
    QCOMPARE(qtSink.frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(qtSink.frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(ndiSink.frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(ndiSink.frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(qtSink.frames[0].video.ptsMs, qint64(100));
    QCOMPARE(qtSink.frames[1].video.ptsMs, qint64(100));
    QCOMPARE(uchar(qtSink.frames[1].video.planeY.at(0)), uchar(40));
    QCOMPARE(ndiSink.frames[0].video.planeY, qtSink.frames[0].video.planeY);
    QCOMPARE(ndiSink.frames[1].video.outputFrameIndex, qtSink.frames[1].video.outputFrameIndex);
}

void TestOutputDispatcher::playingTicksCreateStableOutputPlayEpoch() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 1000, 30));
    cache.insertVideoFrame(video(0, 1040, 60));

    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{qt, &sink}});

    dispatcher.dispatchTick(cache, state);
    dispatcher.dispatchTick(cache, state);

    QCOMPARE(sink.frames.size(), 2);
    QCOMPARE(sink.frames[0].sampledPlayheadMs, qint64(1000));
    QCOMPARE(sink.frames[1].sampledPlayheadMs, qint64(1040));
    QCOMPARE(uchar(sink.frames[0].video.planeY.at(0)), uchar(30));
    QCOMPARE(uchar(sink.frames[1].video.planeY.at(0)), uchar(60));
}

void TestOutputDispatcher::resetPlayEpochKeepsOutputFrameIndexContinuous() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 1000, 30));
    cache.insertVideoFrame(video(0, 5000, 90));

    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{qt, &sink}});

    dispatcher.dispatchTick(cache, state);
    state.playheadMs = 5000;
    dispatcher.resetPlayEpoch();
    dispatcher.dispatchTick(cache, state);

    QCOMPARE(sink.frames.size(), 2);
    QCOMPARE(sink.frames[0].outputFrameIndex, qint64(0));
    QCOMPARE(sink.frames[1].outputFrameIndex, qint64(1));
    QCOMPARE(sink.frames[1].sampledPlayheadMs, qint64(5000));
    QCOMPARE(uchar(sink.frames[1].video.planeY.at(0)), uchar(90));
}

void TestOutputDispatcher::rendersFeedMultiviewAndPgmAssignmentsFromSameTick() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 80, 10));
    cache.insertVideoFrame(video(1, 80, 20));

    PlaybackStateSnapshot state;
    state.playheadMs = 80;
    state.playing = false;
    state.selectedFeedIndex = 1;

    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::Ndi;
    feed0.enabled = true;

    OutputTargetAssignment mv;
    mv.id = QStringLiteral("multiview");
    mv.sourceBus = OutputBusId::multiview();
    mv.kind = OutputTargetKind::QtPreview;
    mv.enabled = true;

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::Ndi;
    pgm.enabled = true;

    CollectingSink feedSink(OutputTargetKind::Ndi);
    CollectingSink mvSink(OutputTargetKind::QtPreview);
    CollectingSink pgmSink(OutputTargetKind::Ndi);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 2, 8, 4);
    dispatcher.setEndpoints({{feed0, &feedSink}, {mv, &mvSink}, {pgm, &pgmSink}});

    dispatcher.dispatchTick(cache, state);

    QCOMPARE(feedSink.frames.size(), 1);
    QCOMPARE(mvSink.frames.size(), 1);
    QCOMPARE(pgmSink.frames.size(), 1);
    QCOMPARE(feedSink.frames[0].bus, OutputBusId::feed(0));
    QCOMPARE(mvSink.frames[0].bus, OutputBusId::multiview());
    QCOMPARE(pgmSink.frames[0].bus, OutputBusId::pgm());
    QCOMPARE(uchar(feedSink.frames[0].video.planeY.at(0)), uchar(10));
    QCOMPARE(uchar(mvSink.frames[0].video.planeY.at(0)), uchar(10));
    QCOMPARE(uchar(mvSink.frames[0].video.planeY.at(4)), uchar(20));
    QCOMPARE(uchar(pgmSink.frames[0].video.planeY.at(0)), uchar(20));
    QCOMPARE(feedSink.frames[0].outputFrameIndex, pgmSink.frames[0].outputFrameIndex);
}

void TestOutputDispatcher::disabledAssignmentsDoNotSubmit() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 0, 50));

    OutputTargetAssignment disabled;
    disabled.id = QStringLiteral("feed0-disabled");
    disabled.sourceBus = OutputBusId::feed(0);
    disabled.kind = OutputTargetKind::Ndi;
    disabled.enabled = false;

    CollectingSink sink(OutputTargetKind::Ndi);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{disabled, &sink}});
    dispatcher.dispatchTick(cache, PlaybackStateSnapshot{});

    QCOMPARE(sink.frames.size(), 0);
}

QTEST_GUILESS_MAIN(TestOutputDispatcher)
#include "tst_outputdispatcher.moc"
