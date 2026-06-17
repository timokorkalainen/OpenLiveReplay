#include <QtTest>

#include "playback/output/yuv420pcompositor.h"

static MediaVideoFrame solid(int feed, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    return f;
}

class TestYuv420pCompositor : public QObject {
    Q_OBJECT
private slots:
    void twoByTwoGridCopiesFeedLumaIntoQuadrants();
    void missingFeedLeavesBlackTile();
};

void TestYuv420pCompositor::twoByTwoGridCopiesFeedLumaIntoQuadrants() {
    QList<MediaVideoFrame> frames{solid(0, 40), solid(1, 80), solid(2, 120), solid(3, 160)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QCOMPARE(out.width, 8);
    QCOMPARE(out.height, 8);
    QCOMPARE(uchar(out.planeY.at(0)), uchar(40));
    QCOMPARE(uchar(out.planeY.at(4)), uchar(80));
    QCOMPARE(uchar(out.planeY.at(4 * 8)), uchar(120));
    QCOMPARE(uchar(out.planeY.at(4 * 8 + 4)), uchar(160));
}

void TestYuv420pCompositor::missingFeedLeavesBlackTile() {
    QList<MediaVideoFrame> frames{solid(0, 40)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QCOMPARE(uchar(out.planeY.at(0)), uchar(40));
    QCOMPARE(uchar(out.planeY.at(4)), uchar(16));
}

QTEST_GUILESS_MAIN(TestYuv420pCompositor)
#include "tst_yuv420pcompositor.moc"
