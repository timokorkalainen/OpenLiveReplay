#include <QtTest>

#include "playback/output/outputbusengine.h"

static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = pts;
    return f;
}

// Distinct Y AND chroma so PGM selection is provable on every plane (a Y-only
// fixture cannot tell two feeds apart on the U/V planes).
static MediaVideoFrame videoYuv(int feed, qint64 pts, uchar y, uchar u, uchar v) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, u, v);
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
    void pgmIsPixelExactCopyOfSelectedFeed();
    void pausedAudioIsSilenceButVideoRepeats();
    void multiviewComposesFeedsAndCarriesSelectedFeedAudio();
    void ntscAudioUsesRationalSampleBoundaries();
    void ntscAudioSpansStayContiguousAcrossOddPlayEpoch();
    void multiviewVideoIdentityTracksSourceContentNotPlayhead();
    void multiviewMemoReusesCompositeForUnchangedSources();
    void multiviewMemoMatchesUnmemoizedCompositeForDistinctSources();
    void programmeTimecodeTracksPlayheadOnEveryBus();
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

// PIXEL-EXACT PGM SELECTION: the PGM bus must emit the SELECTED feed's pixels
// verbatim (whole Y/U/V planes), and must NOT emit any non-selected feed. The
// existing pgmFollowsSelectedFeed check above probes a single corner pixel, so a
// regression that selects the wrong feed but happens to share that pixel — or a
// chroma-only mis-selection — would slip through. Asserting full-plane equality
// against the selected source and full-plane inequality against the other feed
// makes "wrong PGM source" FAIL deterministically.
void TestOutputBusEngine::pgmIsPixelExactCopyOfSelectedFeed() {
    const MediaVideoFrame feed0 = videoYuv(0, 100, 10, 60, 200);
    const MediaVideoFrame feed1 = videoYuv(1, 100, 30, 90, 170);
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(feed0);
    cache.insertVideoFrame(feed1);

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    auto pgm = engine.renderPgm(5, state, cache);
    QCOMPARE(pgm.bus, OutputBusId::pgm());
    QCOMPARE(pgm.video.feedIndex, 1);

    // Every byte of every plane equals the selected feed (feed 1)...
    QCOMPARE(pgm.video.planeY, feed1.planeY);
    QCOMPARE(pgm.video.planeU, feed1.planeU);
    QCOMPARE(pgm.video.planeV, feed1.planeV);

    // ...and is NOT the non-selected feed (feed 0) on any plane.
    QVERIFY(pgm.video.planeY != feed0.planeY);
    QVERIFY(pgm.video.planeU != feed0.planeU);
    QVERIFY(pgm.video.planeV != feed0.planeV);

    // Selecting feed 0 instead must produce feed 0's planes exactly.
    state.selectedFeedIndex = 0;
    auto pgm0 = engine.renderPgm(6, state, cache);
    QCOMPARE(pgm0.video.feedIndex, 0);
    QCOMPARE(pgm0.video.planeY, feed0.planeY);
    QCOMPARE(pgm0.video.planeU, feed0.planeU);
    QCOMPARE(pgm0.video.planeV, feed0.planeV);
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

void TestOutputBusEngine::multiviewComposesFeedsAndCarriesSelectedFeedAudio() {
    OutputFrameCache cache(4, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));
    cache.insertVideoFrame(video(2, 100, 30));
    cache.insertVideoFrame(video(3, 100, 40));
    cache.insertAudioFrame(audio(0, 9600, 100));
    cache.insertAudioFrame(audio(1, 9600, 200));

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 4, 8, 8);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    auto multiview = engine.renderMultiview(5, state, cache);
    QCOMPARE(multiview.bus, OutputBusId::multiview());
    QCOMPARE(uchar(multiview.video.planeY.at(0)), uchar(10));
    QCOMPARE(uchar(multiview.video.planeY.at(4)), uchar(20));
    QCOMPARE(uchar(multiview.video.planeY.at(4 * 8)), uchar(30));
    QCOMPARE(uchar(multiview.video.planeY.at(4 * 8 + 4)), uchar(40));
    const auto* pcm = reinterpret_cast<const qint16*>(multiview.audio.pcm.constData());
    QCOMPARE(multiview.audio.feedIndex, 1);
    QCOMPARE(pcm[0], qint16(200));
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

void TestOutputBusEngine::ntscAudioSpansStayContiguousAcrossOddPlayEpoch() {
    // At 29.97 (30000/1001) the per-frame sample count alternates 1601/1602.
    // The audio start sample is anchored to the play epoch (playStartedAtOutputFrame),
    // so for an ODD epoch the per-frame count must use the SAME epoch-relative phase as
    // the start, otherwise consecutive frames overlap/gap by one sample forever.
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 0, 10));

    OutputBusEngine engine(FrameRate::fromFraction(30000, 1001), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.playStartedAtOutputFrame = 1; // odd epoch: the failing case
    state.playStartedAtPlayheadMs = 0;

    qint64 prevEnd = -1;
    for (qint64 frameIndex = 1; frameIndex <= 6; ++frameIndex) {
        const auto frame = engine.renderFeed(0, frameIndex, state, cache);
        const qint64 start = frame.audio.startSample;
        const qint64 count = frame.audio.sampleFrames();
        if (prevEnd >= 0) {
            // No gap and no overlap: this frame begins exactly where the last one ended.
            QCOMPARE(start, prevEnd);
        }
        prevEnd = start + count;
    }
}

void TestOutputBusEngine::multiviewVideoIdentityTracksSourceContentNotPlayhead() {
    // During 1x playback the sampled playhead advances every tick. The multiview video
    // identity must reflect the composited SOURCE content, so that when the underlying
    // feeds are frozen (same cached pictures) two consecutive ticks compare equal — and
    // when a source advances, the identity changes.
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 8, 8);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;
    state.playStartedAtOutputFrame = 5;
    state.playStartedAtPlayheadMs = 100;

    const auto a = engine.renderMultiview(5, state, cache);
    const auto b = engine.renderMultiview(6, state, cache);

    // Frozen sources across two playing ticks: video identity is unchanged.
    QCOMPARE(a.identity.videoHash, b.identity.videoHash);
    QCOMPARE(a.identity.sourcePtsMs, b.identity.sourcePtsMs);
    QVERIFY(!a.identity.videoPlaceholder);

    // A new source picture changes the multiview video identity.
    cache.insertVideoFrame(video(1, 140, 21));
    PlaybackStateSnapshot advanced = state;
    advanced.playheadMs = 140;
    const auto c = engine.renderMultiview(7, advanced, cache);
    QVERIFY(c.identity.videoHash != b.identity.videoHash);
}

void TestOutputBusEngine::multiviewMemoReusesCompositeForUnchangedSources() {
    // When the source signature is unchanged, the memo reuses the composited planes
    // instead of re-running the full-resolution grid scale. Replacing the content at the
    // SAME source pts (a test-only operation; in production content is immutable per pts)
    // must therefore NOT change the composite, proving the scale was skipped.
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 8, 8);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    MultiviewComposite memo;
    const auto a = engine.renderMultiview(5, state, cache, &memo);
    QCOMPARE(uchar(a.video.planeY.at(0)), uchar(10));

    cache.insertVideoFrame(video(0, 100, 99)); // same pts, new content
    const auto b = engine.renderMultiview(6, state, cache, &memo);
    QCOMPARE(uchar(b.video.planeY.at(0)), uchar(10)); // reused composite, not 99

    cache.insertVideoFrame(video(0, 140, 77)); // a genuine source advance (new pts)
    PlaybackStateSnapshot advanced = state;
    advanced.playheadMs = 140;
    const auto c = engine.renderMultiview(7, advanced, cache, &memo);
    QCOMPARE(uchar(c.video.planeY.at(0)), uchar(77)); // memo invalidated, recomposited
}

void TestOutputBusEngine::multiviewMemoMatchesUnmemoizedCompositeForDistinctSources() {
    // For genuinely distinct sources, a memoized render must be byte-identical to an
    // unmemoized one.
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 100, 10));
    cache.insertVideoFrame(video(1, 100, 20));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 8, 8);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;
    state.selectedFeedIndex = 0;

    MultiviewComposite memo;
    const auto memoized = engine.renderMultiview(9, state, cache, &memo);
    const auto plain = engine.renderMultiview(9, state, cache, nullptr);
    QCOMPARE(memoized.video.planeY, plain.video.planeY);
    QCOMPARE(memoized.video.planeU, plain.video.planeU);
    QCOMPARE(memoized.video.planeV, plain.video.planeV);
    QCOMPARE(memoized.identity.videoHash, plain.identity.videoHash);
}

// Every output bus stamps a programme timecode = playhead position in 100 ns units, so a
// timecode-carrying sink (NDI) can pass it downstream. Asserted on feed / pgm / multiview.
void TestOutputBusEngine::programmeTimecodeTracksPlayheadOnEveryBus() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(video(0, 200, 10));
    cache.insertVideoFrame(video(1, 200, 20));

    OutputBusEngine engine(FrameRate::fromFraction(30, 1), 2, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 200; // paused → sampled playhead == playhead, deterministic
    state.playing = false;
    state.speed = 1.0;
    state.selectedFeedIndex = 1;

    const auto feed = engine.renderFeed(0, 7, state, cache);
    const auto pgm = engine.renderPgm(7, state, cache);
    const auto mv = engine.renderMultiview(7, state, cache, nullptr);

    // The invariant: programme TC is exactly 10000 × the frame's own sampled playhead.
    QCOMPARE(feed.programmeTimecode100ns, feed.sampledPlayheadMs * 10000);
    QCOMPARE(pgm.programmeTimecode100ns, pgm.sampledPlayheadMs * 10000);
    QCOMPARE(mv.programmeTimecode100ns, mv.sampledPlayheadMs * 10000);

    // …and the concrete value at a 200 ms playhead is 2,000,000 × 100 ns.
    QCOMPARE(feed.programmeTimecode100ns, qint64(200) * 10000);
    QCOMPARE(pgm.programmeTimecode100ns, qint64(200) * 10000);
    QCOMPARE(mv.programmeTimecode100ns, qint64(200) * 10000);

    // It advances with the playhead (a later playhead → a strictly larger timecode).
    state.playheadMs = 500;
    const auto later = engine.renderPgm(8, state, cache);
    QCOMPARE(later.programmeTimecode100ns, qint64(500) * 10000);
    QVERIFY(later.programmeTimecode100ns > pgm.programmeTimecode100ns);
}

QTEST_GUILESS_MAIN(TestOutputBusEngine)
#include "tst_outputbusengine.moc"
