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
    explicit CollectingSink(OutputTargetKind kind, bool failSubmits = false, bool failStart = false)
        : m_kind(kind), m_failSubmits(failSubmits), m_failStart(failStart) {}

    OutputTargetKind kind() const override { return m_kind; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_assignment = assignment;
        m_rate = rate;
        m_active = assignment.enabled && rate.isValid() && !m_failStart;
        return m_active;
    }

    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }

    bool submit(const OutputBusFrame& frame) override {
        if (!m_active) return false;
        if (m_failSubmits) return false;
        frames.append(frame);
        return true;
    }

    QVector<OutputBusFrame> frames;

private:
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
    bool m_failSubmits = false;
    bool m_failStart = false;
};

class TestOutputDispatcher : public QObject {
    Q_OBJECT
private slots:
    void pausedTicksRepeatFramesContinuouslyForEverySink();
    void playingTicksCreateStableOutputPlayEpoch();
    void resetPlayEpochKeepsOutputFrameIndexContinuous();
    void rendersFeedMultiviewAndPgmAssignmentsFromSameTick();
    void targetsOnSameBusReceiveMatchingFrameIdentity();
    void targetStatsTrackRepeatedPayloadsAndFailuresIndependently();
    void startFailuresAreVisibleInTargetStats();
    void pgmSwitchUpdatesIdentityOnNextTick();
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
    QVERIFY(qtSink.frames[0].identity.samePayloadAs(ndiSink.frames[0].identity));
    QVERIFY(qtSink.frames[1].identity.samePayloadAs(ndiSink.frames[1].identity));
    QVERIFY(qtSink.frames[0].identity.samePayloadAs(qtSink.frames[1].identity));
    QVERIFY(qtSink.frames[0].identity.outputFrameIndex !=
            qtSink.frames[1].identity.outputFrameIndex);
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

void TestOutputDispatcher::targetsOnSameBusReceiveMatchingFrameIdentity() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 200, 77));

    PlaybackStateSnapshot state;
    state.playheadMs = 200;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment preview;
    preview.id = QStringLiteral("feed0-preview");
    preview.sourceBus = OutputBusId::feed(0);
    preview.kind = OutputTargetKind::QtPreview;
    preview.enabled = true;

    OutputTargetAssignment ndi;
    ndi.id = QStringLiteral("feed0-ndi");
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;

    CollectingSink previewSink(OutputTargetKind::QtPreview);
    CollectingSink ndiSink(OutputTargetKind::Ndi);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{preview, &previewSink}, {ndi, &ndiSink}});

    dispatcher.dispatchTick(cache, state);

    QCOMPARE(previewSink.frames.size(), 1);
    QCOMPARE(ndiSink.frames.size(), 1);
    QCOMPARE(previewSink.frames[0].identity, ndiSink.frames[0].identity);
    QCOMPARE(previewSink.frames[0].identity.bus, OutputBusId::feed(0));
    QCOMPARE(previewSink.frames[0].identity.outputFrameIndex, qint64(0));
    QCOMPARE(previewSink.frames[0].identity.sampledPlayheadMs, qint64(200));
    QCOMPARE(previewSink.frames[0].identity.sourceFeedIndex, 0);
    QCOMPARE(previewSink.frames[0].identity.sourcePtsMs, qint64(200));
    QVERIFY(!previewSink.frames[0].identity.videoPlaceholder);
    QVERIFY(previewSink.frames[0].identity.audioSilent);
    QVERIFY(previewSink.frames[0].identity.videoHash != 0);
}

void TestOutputDispatcher::targetStatsTrackRepeatedPayloadsAndFailuresIndependently() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 44));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment ok;
    ok.id = QStringLiteral("feed0-ok");
    ok.sourceBus = OutputBusId::feed(0);
    ok.kind = OutputTargetKind::QtPreview;
    ok.enabled = true;

    OutputTargetAssignment failing;
    failing.id = QStringLiteral("feed0-failing");
    failing.sourceBus = OutputBusId::feed(0);
    failing.kind = OutputTargetKind::Ndi;
    failing.enabled = true;

    CollectingSink okSink(OutputTargetKind::QtPreview);
    CollectingSink failingSink(OutputTargetKind::Ndi, true);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{ok, &okSink}, {failing, &failingSink}});

    dispatcher.dispatchTick(cache, state);
    dispatcher.dispatchTick(cache, state);

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.ticks, qint64(2));
    QCOMPARE(stats.framesSubmitted, qint64(2));
    QCOMPARE(stats.sinkFailures, qint64(2));
    QVERIFY(stats.targets.contains(QStringLiteral("feed0-ok")));
    QVERIFY(stats.targets.contains(QStringLiteral("feed0-failing")));

    const OutputTargetDispatchStats okStats = stats.targets.value(QStringLiteral("feed0-ok"));
    QCOMPARE(okStats.attemptedFrames, qint64(2));
    QCOMPARE(okStats.framesSubmitted, qint64(2));
    QCOMPARE(okStats.sinkFailures, qint64(0));
    QCOMPARE(okStats.repeatedPayloadFrames, qint64(1));
    QCOMPARE(okStats.silentAudioFrames, qint64(2));
    QVERIFY(okStats.hasLastIdentity);
    QCOMPARE(okStats.lastIdentity.outputFrameIndex, qint64(1));

    const OutputTargetDispatchStats failingStats =
        stats.targets.value(QStringLiteral("feed0-failing"));
    QCOMPARE(failingStats.attemptedFrames, qint64(2));
    QCOMPARE(failingStats.framesSubmitted, qint64(0));
    QCOMPARE(failingStats.sinkFailures, qint64(2));
    QCOMPARE(failingStats.repeatedPayloadFrames, qint64(1));
    QCOMPARE(failingStats.silentAudioFrames, qint64(2));
    QVERIFY(failingStats.hasLastIdentity);
    QCOMPARE(failingStats.lastIdentity.outputFrameIndex, qint64(1));
}

void TestOutputDispatcher::startFailuresAreVisibleInTargetStats() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 44));

    OutputTargetAssignment failing;
    failing.id = QStringLiteral("feed0-start-failing");
    failing.sourceBus = OutputBusId::feed(0);
    failing.kind = OutputTargetKind::Ndi;
    failing.enabled = true;

    CollectingSink failingSink(OutputTargetKind::Ndi, false, true);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{failing, &failingSink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;
    dispatcher.dispatchTick(cache, state);

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.sinkFailures, qint64(1));
    QVERIFY(stats.targets.contains(QStringLiteral("feed0-start-failing")));
    const OutputTargetDispatchStats failingStats =
        stats.targets.value(QStringLiteral("feed0-start-failing"));
    QCOMPARE(failingStats.attemptedFrames, qint64(0));
    QCOMPARE(failingStats.framesSubmitted, qint64(0));
    QCOMPARE(failingStats.sinkFailures, qint64(1));
    QVERIFY(!failingStats.hasLastIdentity);
    QCOMPARE(failingSink.frames.size(), 0);
}

void TestOutputDispatcher::pgmSwitchUpdatesIdentityOnNextTick() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 11));
    cache.insertVideoFrame(video(1, 100, 99));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment pgm;
    pgm.id = QStringLiteral("pgm-preview");
    pgm.sourceBus = OutputBusId::pgm();
    pgm.kind = OutputTargetKind::QtPreview;
    pgm.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 2, 4, 4);
    dispatcher.setEndpoints({{pgm, &sink}});

    dispatcher.dispatchTick(cache, state);
    state.selectedFeedIndex = 1;
    dispatcher.resetPlayEpoch();
    dispatcher.dispatchTick(cache, state);

    QCOMPARE(sink.frames.size(), 2);
    QCOMPARE(sink.frames[0].identity.sourceFeedIndex, 0);
    QCOMPARE(sink.frames[1].identity.sourceFeedIndex, 1);
    QCOMPARE(sink.frames[0].identity.sourcePtsMs, qint64(100));
    QCOMPARE(sink.frames[1].identity.sourcePtsMs, qint64(100));
    QVERIFY(!sink.frames[0].identity.samePayloadAs(sink.frames[1].identity));
    QCOMPARE(uchar(sink.frames[1].video.planeY.at(0)), uchar(99));

    const OutputTargetDispatchStats target =
        dispatcher.stats().targets.value(QStringLiteral("pgm-preview"));
    QCOMPARE(target.framesSubmitted, qint64(2));
    QCOMPARE(target.repeatedPayloadFrames, qint64(0));
    QCOMPARE(target.lastIdentity.sourceFeedIndex, 1);
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
