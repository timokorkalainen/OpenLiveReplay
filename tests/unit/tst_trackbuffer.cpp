#include <QtTest>
#include "playback/trackbuffer.h"

// A distinct, mappable frame per pts so identity is checkable.
static QVideoFrame makeFrame() {
    QVideoFrameFormat fmt(QSize(16, 16), QVideoFrameFormat::Format_YUV420P);
    return QVideoFrame(fmt);
}

class TestTrackBuffer : public QObject {
    Q_OBJECT
private slots:
    void insertKeepsSortedUniqueAndCap();
    void frameAtPicksLargestLeqPlayhead();
    void frameAtEmptyAndBeforeFirst();
    void hasFrameNearTolerance();
    void trimDropsOutsideRange();
    void capProtectsFillEdge();
};

void TestTrackBuffer::insertKeepsSortedUniqueAndCap() {
    TrackBuffer b;
    // Insert out of order; cap large so nothing drops.
    QVERIFY(b.insert(300, makeFrame(), 100, 0, 100000));
    QVERIFY(b.insert(100, makeFrame(), 100, 0, 100000));
    QVERIFY(b.insert(200, makeFrame(), 100, 0, 100000));
    QCOMPARE(b.size(), 3);
    QCOMPARE(b.oldestPts(), int64_t(100));
    QCOMPARE(b.newestPts(), int64_t(300));
    // Duplicate PTS replaces, not grows.
    QVERIFY(b.insert(200, makeFrame(), 100, 0, 100000));
    QCOMPARE(b.size(), 3);
}

void TestTrackBuffer::frameAtPicksLargestLeqPlayhead() {
    TrackBuffer b;
    b.insert(100, makeFrame(), 100, 0, 100000);
    b.insert(200, makeFrame(), 100, 0, 100000);
    b.insert(300, makeFrame(), 100, 0, 100000);
    QVideoFrame out;
    int64_t pts = -1;
    QVERIFY(b.frameAt(250, out, pts));
    QCOMPARE(pts, int64_t(200));
    QVERIFY(b.frameAt(300, out, pts));
    QCOMPARE(pts, int64_t(300)); // exact
    QVERIFY(b.frameAt(100, out, pts));
    QCOMPARE(pts, int64_t(100));
}

void TestTrackBuffer::frameAtEmptyAndBeforeFirst() {
    TrackBuffer b;
    QVideoFrame out;
    int64_t pts = -1;
    QVERIFY(!b.frameAt(50, out, pts)); // empty
    b.insert(100, makeFrame(), 100, 0, 100000);
    QVERIFY(!b.frameAt(50, out, pts)); // before first
}

void TestTrackBuffer::hasFrameNearTolerance() {
    TrackBuffer b;
    b.insert(1000, makeFrame(), 100, 0, 100000);
    QVERIFY(b.hasFrameNear(1015, 16));  // within 16ms
    QVERIFY(!b.hasFrameNear(1100, 16)); // 100ms away
}

void TestTrackBuffer::trimDropsOutsideRange() {
    TrackBuffer b;
    for (int64_t p = 0; p <= 1000; p += 100)
        b.insert(p, makeFrame(), 100, 0, 100000);
    b.trim(300, 700);
    QCOMPARE(b.oldestPts(), int64_t(300));
    QCOMPARE(b.newestPts(), int64_t(700));
}

void TestTrackBuffer::capProtectsFillEdge() {
    TrackBuffer b;
    // cap=3, playhead near 1000, protect up to 1300 (the fill edge).
    // Insert 4 frames around+ahead of the playhead; the farthest-from-1000
    // BELOW the protect range is evicted, never the protected edge frames.
    b.insert(900, makeFrame(), 3, 1000, 1300);
    b.insert(1000, makeFrame(), 3, 1000, 1300);
    b.insert(1100, makeFrame(), 3, 1000, 1300);
    b.insert(1300, makeFrame(), 3, 1000, 1300); // would be 4; cap evicts 900
    QCOMPARE(b.size(), 3);
    QCOMPARE(b.oldestPts(), int64_t(1000)); // 900 evicted (farthest, unprotected)
    QVERIFY(b.hasFrameNear(1300, 1));       // protected edge kept
}
QTEST_APPLESS_MAIN(TestTrackBuffer)
#include "tst_trackbuffer.moc"
