#include <QtTest>

#include "playback/output/yuv420pcompositor.h"

static MediaVideoFrame solid(int feed, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    return f;
}

// A source with distinct, deterministic Y AND chroma so a swapped/duplicated
// quadrant or a corrupted chroma plane is detectable (a Y-only fixture cannot
// tell two feeds apart on the U/V planes).
static MediaVideoFrame solidYuv(int feed, uchar y, uchar u, uchar v) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, u, v);
    f.feedIndex = feed;
    return f;
}

static MediaVideoFrame patterned(int feed) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, 16, 128, 128);
    f.feedIndex = feed;
    for (int i = 0; i < f.planeY.size(); ++i) {
        f.planeY[i] = char(20 + i);
    }
    return f;
}

// Compute the EXPECTED composited planes for a 2x2 grid of equal-size flat
// sources directly from the YUV420P layout, independent of the compositor's
// own loop. The golden is derived from first principles: tile (col,row) gets
// source index row*columns+col; the chroma planes are half-resolution with
// their own stride, so quadrant boundaries land at width/2 in Y and width/4 in
// the chroma planes. Any quadrant swap (wrong Y/U/V in a tile) or off-by-one
// chroma stride (a chroma byte landing one column/row off) breaks exact
// equality on the affected plane.
struct GoldenPlanes {
    QByteArray y;
    QByteArray u;
    QByteArray v;
};

static GoldenPlanes goldenTwoByTwo(int width, int height, const QList<MediaVideoFrame>& src) {
    // Background is the compositor's neutral fill (Y=16, U=V=128).
    GoldenPlanes g;
    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;
    g.y = QByteArray(width * height, char(16));
    g.u = QByteArray(chromaW * chromaH, char(128));
    g.v = QByteArray(chromaW * chromaH, char(128));

    const int columns = 2;
    const int rows = 2;
    for (int i = 0; i < src.size(); ++i) {
        const uchar yv = uchar(src.at(i).planeY.at(0));
        const uchar uv = uchar(src.at(i).planeU.at(0));
        const uchar vv = uchar(src.at(i).planeV.at(0));
        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * width / columns;
        const int dstY = row * height / rows;
        const int dstRight = (col + 1) * width / columns;
        const int dstBottom = (row + 1) * height / rows;
        for (int yy = dstY; yy < dstBottom; ++yy)
            for (int xx = dstX; xx < dstRight; ++xx)
                g.y[yy * width + xx] = char(yv);

        const int cX = dstX / 2;
        const int cY = dstY / 2;
        const int cRight = (dstRight + 1) / 2;
        const int cBottom = (dstBottom + 1) / 2;
        for (int yy = cY; yy < cBottom; ++yy) {
            for (int xx = cX; xx < cRight; ++xx) {
                g.u[yy * chromaW + xx] = char(uv);
                g.v[yy * chromaW + xx] = char(vv);
            }
        }
    }
    return g;
}

class TestYuv420pCompositor : public QObject {
    Q_OBJECT
private slots:
    void twoByTwoGridCopiesFeedLumaIntoQuadrants();
    void downscalesEachFeedAcrossTheWholeSourceFrame();
    void missingFeedLeavesBlackTile();
    void twoByTwoGridIsPixelExactAgainstGolden();
    void quadrantPlacementIsNotSymmetric();
    void chromaPlanesAreByteExactPerQuadrant();
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

void TestYuv420pCompositor::downscalesEachFeedAcrossTheWholeSourceFrame() {
    QList<MediaVideoFrame> frames{patterned(0), solid(1, 80), solid(2, 120), solid(3, 160)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 4, 4);

    QCOMPARE(out.width, 4);
    QCOMPARE(out.height, 4);
    QCOMPARE(uchar(out.planeY.at(0)), uchar(20));
    QCOMPARE(uchar(out.planeY.at(1)), uchar(22));
    QCOMPARE(uchar(out.planeY.at(4)), uchar(28));
    QCOMPARE(uchar(out.planeY.at(5)), uchar(30));
}

void TestYuv420pCompositor::missingFeedLeavesBlackTile() {
    QList<MediaVideoFrame> frames{solid(0, 40), MediaVideoFrame{}};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QCOMPARE(uchar(out.planeY.at(0)), uchar(40));
    QCOMPARE(uchar(out.planeY.at(4)), uchar(16));
}

// PIXEL-EXACT GOLDEN: four sources with distinct Y AND distinct chroma are
// composited into an 8x8 2x2 grid; every byte of the Y, U and V planes must
// equal a golden computed independently from the YUV420P layout. This is the
// content-correctness gate: a swapped/duplicated quadrant or an off-by-one
// chroma offset changes at least one plane byte and FAILS exact equality —
// unlike the single-corner-pixel probes above, which pass even when the rest of
// a tile is wrong.
void TestYuv420pCompositor::twoByTwoGridIsPixelExactAgainstGolden() {
    QList<MediaVideoFrame> frames{solidYuv(0, 40, 60, 200), solidYuv(1, 80, 70, 190),
                                  solidYuv(2, 120, 80, 180), solidYuv(3, 160, 90, 170)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QCOMPARE(out.width, 8);
    QCOMPARE(out.height, 8);
    // Strides must match the declared plane geometry (an off-by-one stride here
    // would silently shift every row of the asserted planes).
    QCOMPARE(out.strideY, 8);
    QCOMPARE(out.strideU, 4);
    QCOMPARE(out.strideV, 4);

    const GoldenPlanes golden = goldenTwoByTwo(8, 8, frames);
    QCOMPARE(out.planeY, golden.y);
    QCOMPARE(out.planeU, golden.u);
    QCOMPARE(out.planeV, golden.v);
}

// A swapped quadrant must FAIL: composite the SAME four sources but with two
// quadrants exchanged, and assert the result differs from the correct golden on
// at least one plane. This is the positive proof that the pixel-exact gate above
// bites (it is the in-test analogue of the manual mutation in the report).
void TestYuv420pCompositor::quadrantPlacementIsNotSymmetric() {
    QList<MediaVideoFrame> correct{solidYuv(0, 40, 60, 200), solidYuv(1, 80, 70, 190),
                                   solidYuv(2, 120, 80, 180), solidYuv(3, 160, 90, 170)};
    // Swap the top-right (index 1) and bottom-left (index 2) feeds.
    QList<MediaVideoFrame> swapped{correct.at(0), correct.at(2), correct.at(1), correct.at(3)};

    MediaVideoFrame out = Yuv420pCompositor::composeGrid(swapped, 8, 8);
    const GoldenPlanes golden = goldenTwoByTwo(8, 8, correct);

    const bool anyPlaneDiffers =
        out.planeY != golden.y || out.planeU != golden.u || out.planeV != golden.v;
    QVERIFY2(anyPlaneDiffers, "swapped quadrants must not equal the correct golden");
    // And it MUST equal the golden for the swapped arrangement (correctness of the
    // golden model itself, not just inequality).
    const GoldenPlanes swappedGolden = goldenTwoByTwo(8, 8, swapped);
    QCOMPARE(out.planeY, swappedGolden.y);
    QCOMPARE(out.planeU, swappedGolden.u);
    QCOMPARE(out.planeV, swappedGolden.v);
}

// Chroma-plane / stride correctness: each quadrant occupies a 2x2 chroma block
// in a width/2-stride plane. Assert the exact U/V bytes per chroma cell so an
// off-by-one chroma stride or a half-pel-misplaced chroma offset FAILS — the Y
// plane alone cannot catch a chroma-only regression.
void TestYuv420pCompositor::chromaPlanesAreByteExactPerQuadrant() {
    QList<MediaVideoFrame> frames{solidYuv(0, 40, 10, 240), solidYuv(1, 80, 20, 230),
                                  solidYuv(2, 120, 30, 220), solidYuv(3, 160, 40, 210)};
    MediaVideoFrame out = Yuv420pCompositor::composeGrid(frames, 8, 8);

    // 8x8 -> 4x4 chroma plane, stride 4. The four 2x2 chroma quadrants:
    // (cx,cy) in {0,1}x{0,1} columns/rows; tile index = (cy/... ) — here each tile
    // is exactly a 2x2 chroma block at chroma-origin (col*2, row*2).
    const int cStride = out.strideU;
    auto chromaAt = [&](const QByteArray& plane, int cx, int cy) {
        return uchar(plane.at(cy * cStride + cx));
    };
    // Top-left feed (U=10,V=240) fills chroma cells [0..1]x[0..1].
    QCOMPARE(chromaAt(out.planeU, 0, 0), uchar(10));
    QCOMPARE(chromaAt(out.planeU, 1, 1), uchar(10));
    QCOMPARE(chromaAt(out.planeV, 1, 0), uchar(240));
    // Top-right feed (U=20) fills chroma cells [2..3]x[0..1].
    QCOMPARE(chromaAt(out.planeU, 2, 0), uchar(20));
    QCOMPARE(chromaAt(out.planeU, 3, 1), uchar(20));
    QCOMPARE(chromaAt(out.planeV, 2, 1), uchar(230));
    // Bottom-left feed (U=30) fills chroma cells [0..1]x[2..3].
    QCOMPARE(chromaAt(out.planeU, 0, 2), uchar(30));
    QCOMPARE(chromaAt(out.planeU, 1, 3), uchar(30));
    QCOMPARE(chromaAt(out.planeV, 0, 3), uchar(220));
    // Bottom-right feed (U=40) fills chroma cells [2..3]x[2..3].
    QCOMPARE(chromaAt(out.planeU, 2, 2), uchar(40));
    QCOMPARE(chromaAt(out.planeU, 3, 3), uchar(40));
    QCOMPARE(chromaAt(out.planeV, 3, 2), uchar(210));

    // And the whole U/V planes must match the golden (catches any cell the
    // explicit probes above miss).
    const GoldenPlanes golden = goldenTwoByTwo(8, 8, frames);
    QCOMPARE(out.planeU, golden.u);
    QCOMPARE(out.planeV, golden.v);
}

QTEST_GUILESS_MAIN(TestYuv420pCompositor)
#include "tst_yuv420pcompositor.moc"
