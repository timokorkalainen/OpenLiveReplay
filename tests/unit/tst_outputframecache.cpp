#include <QtTest>

#include "playback/output/outputframecache.h"

static MediaVideoFrame makeVideo(int feed, qint64 ptsMs, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = ptsMs;
    return f;
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
};

void TestOutputFrameCache::videoAtPicksLargestPtsAtOrBeforePlayhead() {
    OutputFrameCache cache(2, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 10));
    cache.insertVideoFrame(makeVideo(0, 200, 20));
    cache.insertVideoFrame(makeVideo(0, 300, 30));

    auto frame = cache.videoFrameAt(0, 250);
    QVERIFY(frame.has_value());
    QCOMPARE(frame->ptsMs, qint64(200));
    QCOMPARE(uchar(frame->planeY.at(0)), uchar(20));
}

void TestOutputFrameCache::videoFallsBackToLastValidFrame() {
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(makeVideo(0, 100, 42));
    auto frame = cache.videoFrameAt(0, 2000);
    QVERIFY(frame.has_value());
    QCOMPARE(frame->ptsMs, qint64(100));
    QCOMPARE(uchar(frame->planeY.at(0)), uchar(42));
}

void TestOutputFrameCache::missingVideoReturnsPlaceholder() {
    OutputFrameCache cache(1, 4, 4);
    auto frame = cache.videoFrameOrPlaceholder(0, 0);
    QCOMPARE(frame.feedIndex, 0);
    QCOMPARE(frame.width, 4);
    QCOMPARE(frame.height, 4);
    QVERIFY(frame.isPlaceholder);
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
    QCOMPARE(boundary->ptsMs, qint64(100));
    QCOMPARE(uchar(boundary->planeY.at(0)), uchar(20));

    auto current = cache.videoFrameAt(0, 250);
    QVERIFY(current.has_value());
    QCOMPARE(current->ptsMs, qint64(200));
    QCOMPARE(uchar(current->planeY.at(0)), uchar(30));
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

    MediaVideoFrame frame = cache.videoFrameOrPlaceholder(0, 100);
    QVERIFY(frame.isPlaceholder);
    QCOMPARE(cache.audioSpanOrSilence(0, 100, 4), silentS16Stereo(4));
}

void TestOutputFrameCache::mergeFromInsertsByPtsWithoutClearing() {
    OutputFrameCache live(1, 4, 4);
    MediaVideoFrame oldF = MediaVideoFrame::solidYuv420p(4, 4, 10, 128, 128);
    oldF.feedIndex = 0;
    oldF.ptsMs = 5000;
    live.insertVideoFrame(oldF);

    OutputFrameCache staging(1, 4, 4);
    MediaVideoFrame newF = MediaVideoFrame::solidYuv420p(4, 4, 20, 128, 128);
    newF.feedIndex = 0;
    newF.ptsMs = 200;
    staging.insertVideoFrame(newF);

    live.mergeFrom(staging);

    // Both old and new frames survive the merge (no clear).
    auto atNew = live.videoFrameAt(0, 200);
    auto atOld = live.videoFrameAt(0, 5000);
    QVERIFY(atNew.has_value());
    QVERIFY(atOld.has_value());
    QCOMPARE(int(uchar(atNew->planeY.at(0))), 20);
    QCOMPARE(int(uchar(atOld->planeY.at(0))), 10);
}

QTEST_GUILESS_MAIN(TestOutputFrameCache)
#include "tst_outputframecache.moc"
