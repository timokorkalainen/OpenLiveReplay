#include <QtTest>
#include "playback/audioframequeue.h"

static QByteArray pcm(int bytes) { return QByteArray(bytes, '\0'); }

class TestAudioFrameQueue : public QObject {
    Q_OBJECT
private slots:
    void releasesOnlyWithinLead();
    void releaseIsFifo();
    void dropOlderThanBoundsMemory();
    void clearEmpties();
};

void TestAudioFrameQueue::releasesOnlyWithinLead() {
    AudioFrameQueue q;
    q.enqueue(1000, pcm(8).constData(), 8);
    q.enqueue(1100, pcm(8).constData(), 8);
    q.enqueue(1400, pcm(8).constData(), 8);
    AudioFrameQueue::Frame f;
    // playhead 1000, lead 200 -> releases up to pts 1200: 1000 then 1100.
    QVERIFY(q.releaseDue(1000, 200, f)); QCOMPARE(f.ptsMs, int64_t(1000));
    QVERIFY(q.releaseDue(1000, 200, f)); QCOMPARE(f.ptsMs, int64_t(1100));
    QVERIFY(!q.releaseDue(1000, 200, f)); // 1400 > 1200, not due
    // playhead advances; 1400 becomes due.
    QVERIFY(q.releaseDue(1300, 200, f)); QCOMPARE(f.ptsMs, int64_t(1400));
}

void TestAudioFrameQueue::releaseIsFifo() {
    AudioFrameQueue q;
    q.enqueue(100, pcm(4).constData(), 4);
    q.enqueue(200, pcm(4).constData(), 4);
    AudioFrameQueue::Frame f;
    QVERIFY(q.releaseDue(10000, 0, f)); QCOMPARE(f.ptsMs, int64_t(100));
    QVERIFY(q.releaseDue(10000, 0, f)); QCOMPARE(f.ptsMs, int64_t(200));
}

void TestAudioFrameQueue::dropOlderThanBoundsMemory() {
    AudioFrameQueue q;
    for (int64_t p = 0; p <= 1000; p += 100) q.enqueue(p, pcm(4).constData(), 4);
    q.dropOlderThan(900, 200); // keep pts >= 700
    AudioFrameQueue::Frame f;
    QVERIFY(q.releaseDue(10000, 0, f)); QCOMPARE(f.ptsMs, int64_t(700));
}

void TestAudioFrameQueue::clearEmpties() {
    AudioFrameQueue q;
    q.enqueue(1, pcm(4).constData(), 4);
    q.clear();
    QVERIFY(q.isEmpty());
}
QTEST_APPLESS_MAIN(TestAudioFrameQueue)
#include "tst_audioframequeue.moc"
