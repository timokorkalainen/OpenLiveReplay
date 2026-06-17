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

QTEST_GUILESS_MAIN(TestOutputFrameCache)
#include "tst_outputframecache.moc"
