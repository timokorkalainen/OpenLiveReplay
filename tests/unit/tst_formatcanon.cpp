#include <QtTest>

#include "playback/output/colormetadata.h"
#include "playback/output/formatcanon.h"
#include "playback/output/framehandle.h"
#include "playback/output/framepixelformat.h"
#include "playback/output/yuv420pcompositor.h"

#include <tuple>

namespace {

CpuPlanes makeNv12_4x4() {
    CpuPlanes planes;
    planes.format = FramePixelFormat::Nv12;
    planes.width = 4;
    planes.height = 4;
    planes.stride[0] = 4;
    planes.stride[1] = 4;
    planes.plane[0] = QByteArray(16, char(100));

    QByteArray uv(8, 0);
    const uchar us[4] = {10, 11, 12, 13};
    const uchar vs[4] = {200, 201, 202, 203};
    for (int i = 0; i < 4; ++i) {
        uv[2 * i] = char(us[i]);
        uv[2 * i + 1] = char(vs[i]);
    }
    planes.plane[1] = uv;
    return planes;
}

formatcanon::Rgb8 expectedSolidRgb(uchar y, uchar u, uchar v, ColorMetadata color) {
    return formatcanon::yuvToRgb8(y, u, v, color.matrix, color.range);
}

bool sameRgb(formatcanon::Rgb8 lhs, formatcanon::Rgb8 rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

} // namespace

class TestFormatCanon : public QObject {
    Q_OBJECT
private slots:
    void planeShapeMatchesYuv420pLayout();
    void planeShapeMatchesNv12Layout();
    void planeShapeMatchesRgba8Layout();
    void absentPlanesAreZero();
    void packedStrideIsWidthTimesBytesPerSample();
    void nv12DeinterleaveProducesPlanarUV();
    void nv12RoundTripsByteExact();
    void chromaUpsampleReplicates2x2Blocks();
    void yuvToRgbVideoRangeNeutralGrey();
    void yuvToRgbPrimariesDifferByMatrix();
    void yuvToRgbClampsAndFullRange();
    void referenceGridPlacesTilesLikeCpuOracleLuma();
    void referenceGridModelsNv12ChromaDecimation();
    void referenceGridOutputIsRgba8();
};

void TestFormatCanon::planeShapeMatchesYuv420pLayout() {
    using namespace formatcanon;
    QCOMPARE(planeCount(FramePixelFormat::Yuv420p), 3);
    const PlaneShape y = planeShape(FramePixelFormat::Yuv420p, 8, 6, 0);
    QCOMPARE(y.width, 8);
    QCOMPARE(y.height, 6);
    const PlaneShape u = planeShape(FramePixelFormat::Yuv420p, 8, 6, 1);
    QCOMPARE(u.width, 4);
    QCOMPARE(u.height, 3);
    const PlaneShape v = planeShape(FramePixelFormat::Yuv420p, 8, 6, 2);
    QCOMPARE(v.width, 4);
    QCOMPARE(v.height, 3);
    QCOMPARE(planeShape(FramePixelFormat::Yuv420p, 5, 5, 1).width, 3);
    QCOMPARE(planeShape(FramePixelFormat::Yuv420p, 5, 5, 1).height, 3);
}

void TestFormatCanon::planeShapeMatchesNv12Layout() {
    using namespace formatcanon;
    QCOMPARE(planeCount(FramePixelFormat::Nv12), 2);
    const PlaneShape y = planeShape(FramePixelFormat::Nv12, 8, 6, 0);
    QCOMPARE(y.width, 8);
    QCOMPARE(y.height, 6);
    const PlaneShape uv = planeShape(FramePixelFormat::Nv12, 8, 6, 1);
    QCOMPARE(uv.width, 8);
    QCOMPARE(uv.height, 3);
    QCOMPARE(bytesPerSample(FramePixelFormat::Nv12, 1), 1);
}

void TestFormatCanon::planeShapeMatchesRgba8Layout() {
    using namespace formatcanon;
    QCOMPARE(planeCount(FramePixelFormat::Rgba8), 1);
    const PlaneShape p = planeShape(FramePixelFormat::Rgba8, 8, 6, 0);
    QCOMPARE(p.width, 8);
    QCOMPARE(p.height, 6);
    QCOMPARE(bytesPerSample(FramePixelFormat::Rgba8, 0), 4);
    QCOMPARE(packedStride(FramePixelFormat::Rgba8, 8, 6, 0), 32);
}

void TestFormatCanon::absentPlanesAreZero() {
    using namespace formatcanon;
    const PlaneShape p = planeShape(FramePixelFormat::Rgba8, 8, 6, 1);
    QCOMPARE(p.width, 0);
    QCOMPARE(p.height, 0);
    QCOMPARE(planeShape(FramePixelFormat::Nv12, 8, 6, 2).width, 0);
}

void TestFormatCanon::packedStrideIsWidthTimesBytesPerSample() {
    using namespace formatcanon;
    QCOMPARE(packedStride(FramePixelFormat::Yuv420p, 8, 6, 0), 8);
    QCOMPARE(packedStride(FramePixelFormat::Yuv420p, 8, 6, 1), 4);
    QCOMPARE(packedStride(FramePixelFormat::Nv12, 8, 6, 1), 8);
}

void TestFormatCanon::nv12DeinterleaveProducesPlanarUV() {
    const CpuPlanes out = formatcanon::nv12ToYuv420p(makeNv12_4x4());
    QVERIFY(out.isValid());
    QCOMPARE(int(out.format), int(FramePixelFormat::Yuv420p));
    QCOMPARE(out.width, 4);
    QCOMPARE(out.height, 4);
    QCOMPARE(out.stride[1], 2);
    QCOMPARE(out.stride[2], 2);
    QCOMPARE(out.plane[0], QByteArray(16, char(100)));

    const uchar usExp[4] = {10, 11, 12, 13};
    const uchar vsExp[4] = {200, 201, 202, 203};
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(uchar(out.plane[1].at(i)), usExp[i]);
        QCOMPARE(uchar(out.plane[2].at(i)), vsExp[i]);
    }
}

void TestFormatCanon::nv12RoundTripsByteExact() {
    const CpuPlanes nv12 = makeNv12_4x4();
    const CpuPlanes planar = formatcanon::nv12ToYuv420p(nv12);
    const CpuPlanes back = formatcanon::yuv420pToNv12(planar);
    QVERIFY(back.isValid());
    QCOMPARE(int(back.format), int(FramePixelFormat::Nv12));
    QCOMPARE(back.plane[0], nv12.plane[0]);
    QCOMPARE(back.plane[1], nv12.plane[1]);
    QVERIFY(!formatcanon::nv12ToYuv420p(planar).isValid());
    QVERIFY(!formatcanon::yuv420pToNv12(nv12).isValid());
}

void TestFormatCanon::chromaUpsampleReplicates2x2Blocks() {
    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = 4;
    planes.height = 4;
    planes.stride[0] = 4;
    planes.stride[1] = 2;
    planes.stride[2] = 2;
    planes.plane[0] = QByteArray(16, char(0));
    planes.plane[1] = QByteArrayLiteral("\x0a\x14\x1e\x28");
    planes.plane[2] = QByteArray(4, 0);
    planes.plane[2][0] = char(110);
    planes.plane[2][1] = char(120);
    planes.plane[2][2] = char(130);
    planes.plane[2][3] = char(140);

    const formatcanon::FullResChroma chroma = formatcanon::upsampleChromaNearest(planes);
    QCOMPARE(chroma.width, 4);
    QCOMPARE(chroma.height, 4);
    auto uAt = [&](int x, int y) { return uchar(chroma.u.at(y * 4 + x)); };
    auto vAt = [&](int x, int y) { return uchar(chroma.v.at(y * 4 + x)); };

    QCOMPARE(uAt(0, 0), uchar(10));
    QCOMPARE(uAt(1, 1), uchar(10));
    QCOMPARE(vAt(0, 0), uchar(110));
    QCOMPARE(vAt(1, 1), uchar(110));
    QCOMPARE(uAt(2, 0), uchar(20));
    QCOMPARE(uAt(3, 1), uchar(20));
    QCOMPARE(vAt(3, 0), uchar(120));
    QCOMPARE(uAt(0, 2), uchar(30));
    QCOMPARE(uAt(1, 3), uchar(30));
    QCOMPARE(uAt(2, 2), uchar(40));
    QCOMPARE(uAt(3, 3), uchar(40));
    QCOMPARE(vAt(2, 3), uchar(140));
}

void TestFormatCanon::yuvToRgbVideoRangeNeutralGrey() {
    const formatcanon::Rgb8 grey =
        formatcanon::yuvToRgb8(126, 128, 128, ColorMatrix::Bt709, ColorRange::Video);
    QCOMPARE(grey.r, grey.g);
    QCOMPARE(grey.g, grey.b);

    const formatcanon::Rgb8 black =
        formatcanon::yuvToRgb8(16, 128, 128, ColorMatrix::Bt709, ColorRange::Video);
    QCOMPARE(black.r, uchar(0));
    QCOMPARE(black.g, uchar(0));
    QCOMPARE(black.b, uchar(0));

    const formatcanon::Rgb8 white =
        formatcanon::yuvToRgb8(235, 128, 128, ColorMatrix::Bt709, ColorRange::Video);
    QCOMPARE(white.r, uchar(255));
    QCOMPARE(white.g, uchar(255));
    QCOMPARE(white.b, uchar(255));
}

void TestFormatCanon::yuvToRgbPrimariesDifferByMatrix() {
    const formatcanon::Rgb8 c601 =
        formatcanon::yuvToRgb8(120, 90, 200, ColorMatrix::Bt601, ColorRange::Video);
    const formatcanon::Rgb8 c709 =
        formatcanon::yuvToRgb8(120, 90, 200, ColorMatrix::Bt709, ColorRange::Video);
    QVERIFY(c601.r != c709.r || c601.g != c709.g || c601.b != c709.b);
}

void TestFormatCanon::yuvToRgbClampsAndFullRange() {
    const formatcanon::Rgb8 black =
        formatcanon::yuvToRgb8(0, 128, 128, ColorMatrix::Bt709, ColorRange::Full);
    QCOMPARE(black.r, uchar(0));
    QCOMPARE(black.g, uchar(0));
    QCOMPARE(black.b, uchar(0));

    const formatcanon::Rgb8 white =
        formatcanon::yuvToRgb8(255, 128, 128, ColorMatrix::Bt709, ColorRange::Full);
    QCOMPARE(white.r, uchar(255));
    QCOMPARE(white.g, uchar(255));
    QCOMPARE(white.b, uchar(255));

    const formatcanon::Rgb8 hot =
        formatcanon::yuvToRgb8(235, 16, 240, ColorMatrix::Bt601, ColorRange::Video);
    QCOMPARE(hot.r, uchar(255));
    QVERIFY(hot.b < hot.r);
}

void TestFormatCanon::referenceGridPlacesTilesLikeCpuOracleLuma() {
    QList<FrameHandle> frames{
        solidYuv420pHandle(4, 4, 40, 60, 200), solidYuv420pHandle(4, 4, 80, 70, 190),
        solidYuv420pHandle(4, 4, 120, 80, 180), solidYuv420pHandle(4, 4, 160, 90, 170)};
    ColorMetadata color;
    const CpuPlanes rgba = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    QVERIFY(rgba.isValid());
    QCOMPARE(int(rgba.format), int(FramePixelFormat::Rgba8));
    QCOMPARE(rgba.width, 8);
    QCOMPARE(rgba.height, 8);

    auto pixelRgb = [&](int x, int y) {
        const int offset = y * rgba.stride[0] + x * 4;
        return formatcanon::Rgb8{uchar(rgba.plane[0].at(offset)),
                                 uchar(rgba.plane[0].at(offset + 1)),
                                 uchar(rgba.plane[0].at(offset + 2))};
    };

    QVERIFY(sameRgb(pixelRgb(0, 0), expectedSolidRgb(40, 60, 200, color)));
    QVERIFY(sameRgb(pixelRgb(4, 0), expectedSolidRgb(80, 70, 190, color)));
    QVERIFY(sameRgb(pixelRgb(0, 4), expectedSolidRgb(120, 80, 180, color)));
    QVERIFY(sameRgb(pixelRgb(4, 4), expectedSolidRgb(160, 90, 170, color)));
    QCOMPARE(uchar(rgba.plane[0].at(3)), uchar(255));
}

void TestFormatCanon::referenceGridModelsNv12ChromaDecimation() {
    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = 2;
    planes.height = 2;
    planes.stride[0] = 2;
    planes.stride[1] = 1;
    planes.stride[2] = 1;
    planes.plane[0] = QByteArray(4, char(128));
    planes.plane[1] = QByteArray(1, char(64));
    planes.plane[2] = QByteArray(1, char(192));
    FrameHandle handle = makeCpuFrameHandle(planes, FrameMetadata{});

    ColorMetadata color;
    const CpuPlanes rgba = formatcanon::referenceComposeGridRgba8({handle}, 2, 2, color);
    QVERIFY(rgba.isValid());
    auto px = [&](int x, int y) {
        const int offset = y * rgba.stride[0] + x * 4;
        return std::make_tuple(uchar(rgba.plane[0].at(offset)), uchar(rgba.plane[0].at(offset + 1)),
                               uchar(rgba.plane[0].at(offset + 2)));
    };
    QCOMPARE(px(0, 0), px(1, 1));
    QCOMPARE(px(1, 0), px(0, 1));
    QCOMPARE(px(0, 0), px(1, 0));
}

void TestFormatCanon::referenceGridOutputIsRgba8() {
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 100, 128, 128)};
    const CpuPlanes rgba = formatcanon::referenceComposeGridRgba8(frames, 4, 4, ColorMetadata{});
    QVERIFY(rgba.isValid());
    QCOMPARE(int(rgba.format), int(FramePixelFormat::Rgba8));
    QCOMPARE(rgba.stride[0], 16);
    QCOMPARE(rgba.plane[0].size(), 4 * 4 * 4);
}

QTEST_GUILESS_MAIN(TestFormatCanon)
#include "tst_formatcanon.moc"
