#include <QtTest>

#include "playback/output/broadcastoutputstatus.h"
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

    FrameRate receivedRate() const { return m_rate; }

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
    void identicalConsecutiveTicksSkipDuplicateSubmit();
    void statsMergeSinkOutputStatusWithDispatchAttempts();
    void dispatchStatsConvertToBroadcastStatuses();
    void startFailuresAreVisibleInTargetStats();
    void pgmSwitchUpdatesIdentityOnNextTick();
    void disabledAssignmentsDoNotSubmit();
    void reverseAndSpeedChangeReanchorPlayhead();
    void playheadJumpWithoutReanchorIsCaughtByClockDivergence();
    void rationalRateIsCarriedToSinkOnStart();
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
    dispatcher.setIdentitySkip(false); // test exercises repeated-submit behavior directly

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
    dispatcher.setIdentitySkip(
        false); // force submits so repeatedPayloadFrames counts on submit path

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

void TestOutputDispatcher::identicalConsecutiveTicksSkipDuplicateSubmit() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 40));

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false; // paused: same playhead+frame every tick
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    CollectingSink qtSink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{qt, &qtSink}});

    dispatcher.dispatchTick(cache, state);                                    // first: real submit
    const OutputDispatchStats after2 = dispatcher.dispatchTick(cache, state); // dup: skipped

    QCOMPARE(qtSink.frames.size(), 1); // only one submit reached the sink
    QCOMPARE(after2.skippedDuplicateFrames, qint64(1));
}

void TestOutputDispatcher::statsMergeSinkOutputStatusWithDispatchAttempts() {
    class StatusReportingSink final : public IOutputSink {
    public:
        OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }
        bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
            m_active = assignment.enabled && rate.isValid();
            return m_active;
        }
        void stop() override { m_active = false; }
        bool isActive() const override { return m_active; }
        bool submit(const OutputBusFrame&) override { return m_active; }
        OutputSinkStatus outputStatus() const override {
            OutputSinkStatus status;
            status.acceptedFrames = 0;
            status.failedFrames = 1;
            status.droppedFrames = 2;
            status.hasLastResult = true;
            status.lastResultSucceeded = false;
            status.state = QStringLiteral("send-failed");
            status.message = QStringLiteral("backend rejected frame");
            status.currentQueueDepth = 1;
            status.maxQueueDepth = 3;
            status.deliveryGaps = 2;
            status.lastQueuedFrameIndex = 12;
            status.lastDeliveredFrameIndex = 11;
            status.queuePressure = true;
            status.lastSubmitDroppedFrame = true;
            status.lastDeliveryGap = true;
            status.hasLastQueuedFrameIndex = true;
            status.hasLastDeliveredFrameIndex = true;
            return status;
        }

    private:
        bool m_active = false;
    };

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 44));

    OutputTargetAssignment ndi;
    ndi.id = QStringLiteral("feed0-ndi");
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;

    StatusReportingSink sink;
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{ndi, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;
    dispatcher.dispatchTick(cache, state);

    const OutputTargetDispatchStats target =
        dispatcher.stats().targets.value(QStringLiteral("feed0-ndi"));
    QCOMPARE(target.attemptedFrames, qint64(1));
    QCOMPARE(target.framesSubmitted, qint64(1));
    QVERIFY(target.hasLastSubmitResult);
    QVERIFY(target.lastSubmitSucceeded);
    QVERIFY(target.hasSinkStatus);
    QCOMPARE(target.sinkSubmittedFrames, qint64(0));
    QCOMPARE(target.sinkFailedFrames, qint64(1));
    QCOMPARE(target.sinkDroppedFrames, qint64(2));
    QVERIFY(target.hasLastSinkResult);
    QVERIFY(!target.lastSinkResultSucceeded);
    QCOMPARE(target.sinkState, QStringLiteral("send-failed"));
    QCOMPARE(target.sinkMessage, QStringLiteral("backend rejected frame"));
    QCOMPARE(target.currentQueueDepth, qint64(1));
    QCOMPARE(target.maxQueueDepth, qint64(3));
    QCOMPARE(target.deliveryGaps, qint64(2));
    QCOMPARE(target.lastQueuedFrameIndex, qint64(12));
    QCOMPARE(target.lastDeliveredFrameIndex, qint64(11));
    QVERIFY(target.queuePressure);
    QVERIFY(target.lastSubmitDroppedFrame);
    QVERIFY(target.lastDeliveryGap);
    QVERIFY(target.hasLastQueuedFrameIndex);
    QVERIFY(target.hasLastDeliveredFrameIndex);
}

void TestOutputDispatcher::dispatchStatsConvertToBroadcastStatuses() {
    OutputDispatchStats stats;
    stats.runtime.deadlineMisses = 2;
    stats.runtime.catchUpCapHits = 3;
    stats.runtime.lastDispatchDeadlineMiss = true;
    stats.runtime.lastCappedCatchUpTicks = 4;
    OutputTargetDispatchStats target;
    target.attemptedFrames = 7;
    target.framesSubmitted = 6;
    target.sinkFailures = 1;
    target.sinkSubmittedFrames = 5;
    target.sinkFailedFrames = 2;
    target.sinkDroppedFrames = 3;
    target.placeholderFrames = 2;
    target.silentAudioFrames = 3;
    target.repeatedPayloadFrames = 4;
    target.hasSinkStatus = true;
    target.hasLastSubmitResult = true;
    target.lastSubmitSucceeded = true;
    target.hasLastSinkResult = true;
    target.lastSinkResultSucceeded = false;
    target.sinkState = QStringLiteral("send-failed");
    target.sinkMessage = QStringLiteral("backend rejected frame");
    target.currentQueueDepth = 4;
    target.maxQueueDepth = 8;
    target.deliveryGaps = 2;
    target.lastQueuedFrameIndex = 19;
    target.lastDeliveredFrameIndex = 18;
    target.lastSubmitDurationNs = 123456;
    target.queuePressure = true;
    target.lastSubmitDroppedFrame = true;
    target.lastDeliveryGap = true;
    target.hasLastQueuedFrameIndex = true;
    target.hasLastDeliveredFrameIndex = true;
    target.hasLastIdentity = true;
    target.lastIdentity.outputFrameIndex = 17;
    target.lastIdentity.sampledPlayheadMs = 2040;
    target.lastIdentity.sourceFeedIndex = 1;
    target.lastIdentity.sourcePtsMs = 2000;
    stats.targets.insert(QStringLiteral("feed1-ndi"), target);

    const QHash<QString, BroadcastOutputTargetStatus> statuses =
        BroadcastOutputStatus::fromDispatchStats(stats);

    QVERIFY(statuses.contains(QStringLiteral("feed1-ndi")));
    const BroadcastOutputTargetStatus status = statuses.value(QStringLiteral("feed1-ndi"));
    QCOMPARE(status.attemptedFrames, qint64(7));
    QCOMPARE(status.framesSubmitted, qint64(6));
    QCOMPARE(status.sinkFailures, qint64(1));
    QCOMPARE(status.sinkSubmittedFrames, qint64(5));
    QCOMPARE(status.sinkFailedFrames, qint64(2));
    QCOMPARE(status.sinkDroppedFrames, qint64(3));
    QCOMPARE(status.placeholderFrames, qint64(2));
    QCOMPARE(status.silentAudioFrames, qint64(3));
    QCOMPARE(status.repeatedPayloadFrames, qint64(4));
    QVERIFY(status.hasSinkStatus);
    QVERIFY(status.hasLastSubmitResult);
    QVERIFY(status.lastSubmitSucceeded);
    QVERIFY(status.hasLastSinkResult);
    QVERIFY(!status.lastSinkResultSucceeded);
    QCOMPARE(status.sinkState, QStringLiteral("send-failed"));
    QCOMPARE(status.sinkMessage, QStringLiteral("backend rejected frame"));
    QCOMPARE(status.currentQueueDepth, qint64(4));
    QCOMPARE(status.maxQueueDepth, qint64(8));
    QCOMPARE(status.deliveryGaps, qint64(2));
    QCOMPARE(status.lastQueuedFrameIndex, qint64(19));
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(18));
    QCOMPARE(status.lastSubmitDurationNs, qint64(123456));
    QVERIFY(status.queuePressure);
    QVERIFY(status.lastSubmitDroppedFrame);
    QVERIFY(status.lastDeliveryGap);
    QVERIFY(status.hasLastQueuedFrameIndex);
    QVERIFY(status.hasLastDeliveredFrameIndex);
    QCOMPARE(status.runtimeDeadlineMisses, qint64(2));
    QCOMPARE(status.runtimeCatchUpCapHits, qint64(3));
    QVERIFY(status.runtimeLastDeadlineMiss);
    QCOMPARE(status.runtimeLastCappedCatchUpTicks, qint64(4));
    QVERIFY(status.hasLastIdentity);
    QCOMPARE(status.lastIdentity.outputFrameIndex, qint64(17));
    QCOMPARE(status.lastIdentity.sampledPlayheadMs, qint64(2040));
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

void TestOutputDispatcher::reverseAndSpeedChangeReanchorPlayhead() {
    OutputFrameCache cache(1, 4, 4);
    for (int i = 0; i < 30; ++i)
        cache.insertVideoFrame(video(0, i * 40, uchar(10 + i)));

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputTargetAssignment a;
    a.id = QStringLiteral("feed0");
    a.sourceBus = OutputBusId::feed(0);
    a.kind = OutputTargetKind::QtPreview;
    a.enabled = true;

    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{a, &sink}});
    // tick 2 re-anchors onto the same source frame as tick 1 (byte-identical payload);
    // identity-skip (default on) would collapse that submit and hide the re-anchor, so
    // disable it here to assert the per-tick sampled playhead.
    dispatcher.setIdentitySkip(false);

    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.playheadMs = 200;
    state.selectedFeedIndex = 0;

    dispatcher.dispatchTick(cache, state); // tick 0: epoch anchors at frame 0, playhead 200
    dispatcher.dispatchTick(cache, state); // tick 1: 200 + 40

    state.speed = -1.0; // speed change forces re-anchor on the next tick
    state.playheadMs = 240;
    dispatcher.dispatchTick(cache, state); // tick 2: re-anchor, playhead 240
    dispatcher.dispatchTick(cache, state); // tick 3: 240 - 40 (reverse)

    QCOMPARE(sink.frames.size(), 4);
    QCOMPARE(sink.frames[0].sampledPlayheadMs, qint64(200));
    QCOMPARE(sink.frames[1].sampledPlayheadMs, qint64(240));
    QCOMPARE(sink.frames[2].sampledPlayheadMs,
             qint64(240)); // re-anchored at 240; a stale epoch would give 120
    QCOMPARE(sink.frames[3].sampledPlayheadMs, qint64(200)); // reverse step
}

// Frame-accuracy guard (maxClockDivergenceMs). The reposition and armed-cut bugs
// both re-based the snapshot playhead WITHOUT re-anchoring the output play epoch,
// so the sampled (output-clock) playhead kept advancing from the OLD anchor: the
// output rendered a frame seconds stale while reporting ZERO placeholder and ZERO
// reposition, so every other gate passed. maxClockDivergenceMs is the detector
// the e2e seekflash/farback/armedcut gates assert; this proves it actually fires
// on a stale epoch and stays quiet once the epoch is re-anchored (the fix).
void TestOutputDispatcher::playheadJumpWithoutReanchorIsCaughtByClockDivergence() {
    OutputFrameCache cache(1, 4, 4);
    // Real frames spanning [1000, 6000] so the cache lookup never placeholders for
    // either the stale (~1s) or the re-anchored (5s) sampled playhead — the guard
    // only measures divergence on non-placeholder frames.
    for (qint64 pts = 1000; pts <= 6000; pts += 1000)
        cache.insertVideoFrame(video(0, pts, uchar(pts / 100)));

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    PlaybackStateSnapshot state;
    state.playheadMs = 1000;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    // --- Stale epoch: a 4s forward jump with NO resetPlayEpoch ---
    CollectingSink staleSink(OutputTargetKind::QtPreview);
    OutputDispatcher stale(FrameRate::fromFraction(25, 1), 1, 4, 4);
    stale.setEndpoints({{qt, &staleSink}});
    stale.dispatchTick(cache, state); // anchor epoch at playhead 1000
    QCOMPARE(stale.stats().maxClockDivergenceMs, qint64(0));
    state.playheadMs = 5000; // seek re-based the playhead, epoch NOT reset
    const OutputDispatchStats staleStats = stale.dispatchTick(cache, state);
    QVERIFY2(staleStats.maxClockDivergenceMs > 1500,
             qPrintable(QStringLiteral("stale epoch should diverge well past the gate, got %1")
                            .arg(staleStats.maxClockDivergenceMs)));

    // --- Re-anchored: the same jump, but resetPlayEpoch() before the next tick ---
    state.playheadMs = 1000;
    CollectingSink okSink(OutputTargetKind::QtPreview);
    OutputDispatcher reanchored(FrameRate::fromFraction(25, 1), 1, 4, 4);
    reanchored.setEndpoints({{qt, &okSink}});
    reanchored.dispatchTick(cache, state); // anchor epoch at playhead 1000
    state.playheadMs = 5000;
    reanchored.resetPlayEpoch(); // the fix: repositionTo / maybeFireScheduledCut
    const OutputDispatchStats okStats = reanchored.dispatchTick(cache, state);
    QVERIFY2(okStats.maxClockDivergenceMs <= 1500,
             qPrintable(QStringLiteral("re-anchored epoch should track the playhead, got %1")
                            .arg(okStats.maxClockDivergenceMs)));
}

void TestOutputDispatcher::rationalRateIsCarriedToSinkOnStart() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 0, 128));

    PlaybackStateSnapshot state;
    state.playheadMs = 0;
    state.playing = false;
    state.selectedFeedIndex = 0;

    OutputTargetAssignment qt;
    qt.id = QStringLiteral("feed0-preview");
    qt.sourceBus = OutputBusId::feed(0);
    qt.kind = OutputTargetKind::QtPreview;
    qt.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(30000, 1001), 1, 4, 4);
    dispatcher.setEndpoints({{qt, &sink}});
    dispatcher.dispatchTick(cache, state);

    QVERIFY(sink.isActive());
    QCOMPARE(sink.receivedRate().numerator, 30000);
    QCOMPARE(sink.receivedRate().denominator, 1001);
}

QTEST_GUILESS_MAIN(TestOutputDispatcher)
#include "tst_outputdispatcher.moc"
