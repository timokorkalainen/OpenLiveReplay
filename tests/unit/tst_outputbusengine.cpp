#include <QtTest>

#include "playback/output/outputbusengine.h"

static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = pts;
    return f;
}

static MediaAudioFrame audio(int feed, qint64 startSample, qint16 value) {
    MediaAudioFrame a;
    a.feedIndex = feed;
    a.startSample = startSample;
    a.sampleRate = 48000;
    a.channels = 2;
    a.format = MediaSampleFormat::S16Interleaved;
    a.pcm.resize(4 * 2 * int(sizeof(qint16)));
    auto* s = reinterpret_cast<qint16*>(a.pcm.data());
    for (int i = 0; i < 8; ++i)
        s[i] = value;
    return a;
}

class TestOutputBusEngine : public QObject {
    Q_OBJECT
private slots:
    void feedBusUsesOwnVideoAndAudioAtOneX();
    void pgmFollowsSelectedFeed();
    void pausedAudioIsSilenceButVideoRepeats();
    void targetAssignmentsDoNotAffectRenderedBusFrames();
    void multiviewComposesFeedsAndCarriesSilence();
    void ntscAudioUsesRationalSampleBoundaries();
};

void TestOutputBusEngine::feedBusUsesOwnVideoAndAudioAtOneX() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));
    cache.insertAudioFrame(audio(0, 4800, 100));
    cache.insertAudioFrame(audio(1, 4800, 200));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    auto feed0 = engine.renderFeed(0, 3, state, cache);
    auto feed1 = engine.renderFeed(1, 3, state, cache);

    QCOMPARE(uchar(feed0.video.planeY.at(0)), uchar(10));
    QCOMPARE(uchar(feed1.video.planeY.at(0)), uchar(20));
    QCOMPARE(reinterpret_cast<const qint16*>(feed0.audio.pcm.constData())[0], qint16(100));
    QCOMPARE(reinterpret_cast<const qint16*>(feed1.audio.pcm.constData())[0], qint16(200));
}

void TestOutputBusEngine::pgmFollowsSelectedFeed() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 30));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    auto pgm = engine.renderPgm(5, state, cache);
    QCOMPARE(pgm.bus, OutputBusId::pgm());
    QCOMPARE(uchar(pgm.video.planeY.at(0)), uchar(30));
}

void TestOutputBusEngine::pausedAudioIsSilenceButVideoRepeats() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 40));
    cache.insertAudioFrame(audio(0, 4800, 500));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.speed = 1.0;

    auto a = engine.renderFeed(0, 10, state, cache);
    auto b = engine.renderFeed(0, 11, state, cache);
    QCOMPARE(a.video.ptsMs, b.video.ptsMs);
    QCOMPARE(uchar(a.video.planeY.at(0)), uchar(40));
    const auto* pcm = reinterpret_cast<const qint16*>(a.audio.pcm.constData());
    QCOMPARE(pcm[0], qint16(0));
}

void TestOutputBusEngine::targetAssignmentsDoNotAffectRenderedBusFrames() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 55));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;

    auto before = engine.renderFeed(0, 1, state, cache);
    OutputTargetAssignment ndi;
    ndi.sourceBus = OutputBusId::feed(0);
    ndi.kind = OutputTargetKind::Ndi;
    ndi.enabled = true;
    engine.setTargetAssignments({ndi});
    auto after = engine.renderFeed(0, 2, state, cache);

    QCOMPARE(uchar(before.video.planeY.at(0)), uchar(after.video.planeY.at(0)));
    QCOMPARE(before.video.ptsMs, after.video.ptsMs);
}

void TestOutputBusEngine::multiviewComposesFeedsAndCarriesSilence() {
    OutputFrameCache cache(4, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));
    cache.insertVideoFrame(video(2, 100, 30));
    cache.insertVideoFrame(video(3, 100, 40));

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 4, 8, 8);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;

    auto multiview = engine.renderMultiview(5, state, cache);
    QCOMPARE(multiview.bus, OutputBusId::multiview());
    QCOMPARE(uchar(multiview.video.planeY.at(0)), uchar(10));
    QCOMPARE(uchar(multiview.video.planeY.at(4)), uchar(20));
    QCOMPARE(uchar(multiview.video.planeY.at(4 * 8)), uchar(30));
    QCOMPARE(uchar(multiview.video.planeY.at(4 * 8 + 4)), uchar(40));
    const auto* pcm = reinterpret_cast<const qint16*>(multiview.audio.pcm.constData());
    QCOMPARE(pcm[0], qint16(0));
}

void TestOutputBusEngine::ntscAudioUsesRationalSampleBoundaries() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 0, 10));
    cache.insertAudioFrame(audio(0, 1601, 111));
    cache.insertAudioFrame(audio(0, 3203, 222));

    OutputBusEngine engine(FrameRate::fromFraction(30000, 1001), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.playStartedAtOutputFrame = 0;
    state.playStartedAtPlayheadMs = 0;

    auto frame1 = engine.renderFeed(0, 1, state, cache);
    QCOMPARE(frame1.audio.startSample, qint64(1601));
    QCOMPARE(frame1.audio.pcm.size(), 1602 * 2 * int(sizeof(qint16)));
    QCOMPARE(reinterpret_cast<const qint16*>(frame1.audio.pcm.constData())[0], qint16(111));

    auto frame2 = engine.renderFeed(0, 2, state, cache);
    QCOMPARE(frame2.audio.startSample, qint64(3203));
    QCOMPARE(frame2.audio.pcm.size(), 1601 * 2 * int(sizeof(qint16)));
    QCOMPARE(reinterpret_cast<const qint16*>(frame2.audio.pcm.constData())[0], qint16(222));
}

QTEST_GUILESS_MAIN(TestOutputBusEngine)
#include "tst_outputbusengine.moc"
