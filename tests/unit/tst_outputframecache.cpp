#include <QtTest>

#include "playback/output/framehandle.h"
#include "playback/output/outputframecache.h"

static FrameHandle makeVideo(int feed, qint64 ptsMs, uchar y) {
    FrameHandle frame = solidYuv420pHandle(4, 4, y, 128, 128);
    frame.metadata().key.feedIndex = feed;
    frame.metadata().key.ptsMs = ptsMs;
    return frame;
}

class TestOutputFrameCache : public QObject {
    Q_OBJECT
private slots:
    void videoAtPicksLargestPtsAtOrBeforePlayhead();
    void videoFallsBackToLastValidFrame();
    void missingVideoReturnsPlaceholder();
    void audioSpanReturnsSamplesAndSilenceForGaps();
    void trimBeforeBoundsVideoHistoryButKeepsBoundaryFrame();
    void trimBeforeDropsExpiredAudioFrames();
    void clearDropsVideoAndAudioHistory();
    void mergeFromInsertsByPtsWithoutClearing();
    void insertVideoFrameReportsReplacement();
    void trimBeforeReportsVideoEvictions();
    void clearReportsVideoEvictions();
    void mergeFromReportsReplacedVideoFrames();
    void freshCoverageRequiresFrameAtOrBeforeTargetWithinTolerance();
};

void TestOutputFrameCache::videoAtPicksLargestPtsAtOrBeforePlayhead() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 10));
    cache.insertVideoFrame(makeVideo(0, 200, 20));
    cache.insertVideoFrame(makeVideo(0, 300, 30));

    auto frame = cache.videoFrameAt(0, 250);
    QVERIFY(frame.has_value());
    MediaVideoFrameView view(*frame);
    QCOMPARE(view.ptsMs, qint64(200));
    QCOMPARE(uchar(view.planeY.at(0)), uchar(20));
}

void TestOutputFrameCache::videoFallsBackToLastValidFrame() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 42));
    auto frame = cache.videoFrameAt(0, 2000);
    QVERIFY(frame.has_value());
    MediaVideoFrameView view(*frame);
    QCOMPARE(view.ptsMs, qint64(100));
    QCOMPARE(uchar(view.planeY.at(0)), uchar(42));
}

void TestOutputFrameCache::missingVideoReturnsPlaceholder() {
    OutputFrameCache cache(1, 4, 4);
    auto frame = cache.videoFrameOrPlaceholder(0, 0);
    MediaVideoFrameView view(frame);
    QCOMPARE(view.feedIndex, 0);
    QCOMPARE(view.width, 4);
    QCOMPARE(view.height, 4);
    QVERIFY(view.isPlaceholder);
}

void TestOutputFrameCache::audioSpanReturnsSamplesAndSilenceForGaps() {
    OutputFrameCache cache(1, 4, 4);
    QByteArray samples;
    samples.resize(4 * 4); // 4 stereo S16 sample frames
    auto* pcm = reinterpret_cast<qint16*>(samples.data());
    pcm[0] = 1;
    pcm[1] = 2;
    pcm[2] = 3;
    pcm[3] = 4;
    pcm[4] = 5;
    pcm[5] = 6;
    pcm[6] = 7;
    pcm[7] = 8;

    MediaAudioFrame audio;
    audio.feedIndex = 0;
    audio.startSample = 100;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm = samples;
    cache.insertAudioFrame(audio);

    const QByteArray span = cache.audioSpanOrSilence(0, 98, 6);
    QCOMPARE(span.size(), 6 * 2 * int(sizeof(qint16)));
    const auto* out = reinterpret_cast<const qint16*>(span.constData());
    QCOMPARE(out[0], qint16(0));
    QCOMPARE(out[1], qint16(0));
    QCOMPARE(out[4], qint16(1));
    QCOMPARE(out[5], qint16(2));
    QCOMPARE(out[10], qint16(7));
    QCOMPARE(out[11], qint16(8));
}

void TestOutputFrameCache::trimBeforeBoundsVideoHistoryButKeepsBoundaryFrame() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 0, 10));
    cache.insertVideoFrame(makeVideo(0, 100, 20));
    cache.insertVideoFrame(makeVideo(0, 200, 30));
    cache.insertVideoFrame(makeVideo(0, 300, 40));

    cache.trimBefore(150, 0);

    auto dropped = cache.videoFrameAt(0, 50);
    QVERIFY(!dropped.has_value());

    auto boundary = cache.videoFrameAt(0, 175);
    QVERIFY(boundary.has_value());
    MediaVideoFrameView boundaryView(*boundary);
    QCOMPARE(boundaryView.ptsMs, qint64(100));
    QCOMPARE(uchar(boundaryView.planeY.at(0)), uchar(20));

    auto current = cache.videoFrameAt(0, 250);
    QVERIFY(current.has_value());
    MediaVideoFrameView currentView(*current);
    QCOMPARE(currentView.ptsMs, qint64(200));
    QCOMPARE(uchar(currentView.planeY.at(0)), uchar(30));
}

void TestOutputFrameCache::trimBeforeDropsExpiredAudioFrames() {
    OutputFrameCache cache(1, 4, 4);

    MediaAudioFrame oldAudio;
    oldAudio.feedIndex = 0;
    oldAudio.startSample = 0;
    oldAudio.sampleRate = 48000;
    oldAudio.channels = 2;
    oldAudio.format = MediaSampleFormat::S16Interleaved;
    oldAudio.pcm.resize(4 * 2 * int(sizeof(qint16)));
    auto* oldPcm = reinterpret_cast<qint16*>(oldAudio.pcm.data());
    for (int i = 0; i < 8; ++i)
        oldPcm[i] = qint16(1);
    cache.insertAudioFrame(oldAudio);

    MediaAudioFrame currentAudio = oldAudio;
    currentAudio.startSample = 100;
    currentAudio.pcm.resize(4 * 2 * int(sizeof(qint16)));
    auto* currentPcm = reinterpret_cast<qint16*>(currentAudio.pcm.data());
    for (int i = 0; i < 8; ++i)
        currentPcm[i] = qint16(2);
    cache.insertAudioFrame(currentAudio);

    cache.trimBefore(0, 100);

    QCOMPARE(cache.audioSpanOrSilence(0, 0, 4), silentS16Stereo(4));
    const QByteArray retained = cache.audioSpanOrSilence(0, 100, 4);
    const auto* out = reinterpret_cast<const qint16*>(retained.constData());
    QCOMPARE(out[0], qint16(2));
    QCOMPARE(out[1], qint16(2));
}

void TestOutputFrameCache::clearDropsVideoAndAudioHistory() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 50));

    MediaAudioFrame audio;
    audio.feedIndex = 0;
    audio.startSample = 100;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm = QByteArray(4 * 2 * int(sizeof(qint16)), '\1');
    cache.insertAudioFrame(audio);

    cache.clear();

    MediaVideoFrameView frame(cache.videoFrameOrPlaceholder(0, 100));
    QVERIFY(frame.isPlaceholder);
    QCOMPARE(cache.audioSpanOrSilence(0, 100, 4), silentS16Stereo(4));
}

void TestOutputFrameCache::mergeFromInsertsByPtsWithoutClearing() {
    OutputFrameCache live(1, 4, 4);
    FrameHandle oldF = makeVideo(0, 5000, 10);
    live.insertVideoFrame(oldF);

    OutputFrameCache staging(1, 4, 4);
    FrameHandle newF = makeVideo(0, 200, 20);
    staging.insertVideoFrame(newF);

    live.mergeFrom(staging);

    // Both old and new frames survive the merge (no clear).
    auto atNew = live.videoFrameAt(0, 200);
    auto atOld = live.videoFrameAt(0, 5000);
    QVERIFY(atNew.has_value());
    QVERIFY(atOld.has_value());
    QCOMPARE(int(uchar(MediaVideoFrameView(*atNew).planeY.at(0))), 20);
    QCOMPARE(int(uchar(MediaVideoFrameView(*atOld).planeY.at(0))), 10);
}

void TestOutputFrameCache::insertVideoFrameReportsReplacement() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 10));

    OutputFrameCache::EvictedVideoFrames evicted;
    cache.insertVideoFrame(makeVideo(0, 100, 90), &evicted);

    QCOMPARE(evicted.size(), 1);
    QCOMPARE(evicted.first().metadata().key.ptsMs, qint64(100));
    QCOMPARE(uchar(MediaVideoFrameView(evicted.first()).planeY.at(0)), uchar(10));

    auto current = cache.videoFrameAt(0, 100);
    QVERIFY(current.has_value());
    QCOMPARE(uchar(MediaVideoFrameView(*current).planeY.at(0)), uchar(90));
}

void TestOutputFrameCache::trimBeforeReportsVideoEvictions() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 0, 10));
    cache.insertVideoFrame(makeVideo(0, 100, 20));
    cache.insertVideoFrame(makeVideo(0, 200, 30));
    cache.insertVideoFrame(makeVideo(0, 300, 40));

    OutputFrameCache::EvictedVideoFrames evicted;
    cache.trimBefore(250, 0, &evicted);

    QCOMPARE(evicted.size(), 2);
    QCOMPARE(evicted[0].metadata().key.ptsMs, qint64(0));
    QCOMPARE(evicted[1].metadata().key.ptsMs, qint64(100));

    auto boundary = cache.videoFrameAt(0, 250);
    QVERIFY(boundary.has_value());
    QCOMPARE(boundary->metadata().key.ptsMs, qint64(200));
}

void TestOutputFrameCache::clearReportsVideoEvictions() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 10));
    cache.insertVideoFrame(makeVideo(0, 200, 20));

    OutputFrameCache::EvictedVideoFrames evicted;
    cache.clear(&evicted);

    QCOMPARE(evicted.size(), 2);
    QCOMPARE(evicted[0].metadata().key.ptsMs, qint64(100));
    QCOMPARE(evicted[1].metadata().key.ptsMs, qint64(200));
    QVERIFY(cache.videoFrameOrPlaceholder(0, 100).metadata().key.isPlaceholder);
}

void TestOutputFrameCache::mergeFromReportsReplacedVideoFrames() {
    OutputFrameCache live(1, 4, 4);
    live.insertVideoFrame(makeVideo(0, 100, 10));

    OutputFrameCache staging(1, 4, 4);
    staging.insertVideoFrame(makeVideo(0, 100, 80));

    OutputFrameCache::EvictedVideoFrames evicted;
    live.mergeFrom(staging, &evicted);

    QCOMPARE(evicted.size(), 1);
    QCOMPARE(uchar(MediaVideoFrameView(evicted.first()).planeY.at(0)), uchar(10));

    auto current = live.videoFrameAt(0, 100);
    QVERIFY(current.has_value());
    QCOMPARE(uchar(MediaVideoFrameView(*current).planeY.at(0)), uchar(80));
}

void TestOutputFrameCache::freshCoverageRequiresFrameAtOrBeforeTargetWithinTolerance() {
    OutputFrameCache cache(1, 4, 4);
    FrameHandle old = makeVideo(0, 90, 10);
    old.metadata().gpuGeneration = 1;
    FrameHandle exact = makeVideo(0, 100, 20);
    exact.metadata().gpuGeneration = 1;
    FrameHandle stale = makeVideo(0, 140, 30);
    stale.metadata().gpuGeneration = 1;
    FrameHandle future = makeVideo(0, 220, 40);
    future.metadata().gpuGeneration = 2;
    cache.insertVideoFrame(old);
    cache.insertVideoFrame(exact);
    cache.insertVideoFrame(stale);
    cache.insertVideoFrame(future);

    QVERIFY(cache.hasFreshVideoFrameAtOrBeforeNear(0, 100, 15, 1));
    QVERIFY(cache.hasFreshVideoFrameAtOrBeforeNear(0, 105, 15, 1));
    QVERIFY(!cache.hasFreshVideoFrameAtOrBeforeNear(0, 140, 15, 2));
    QVERIFY(!cache.hasFreshVideoFrameAtOrBeforeNear(0, 210, 15, 2));
    QVERIFY(!cache.hasFreshVideoFrameAtOrBeforeNear(0, 80, 15, 1));
}

QTEST_GUILESS_MAIN(TestOutputFrameCache)
#include "tst_outputframecache.moc"
