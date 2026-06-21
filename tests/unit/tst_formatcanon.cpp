#include <QtTest>

#include "playback/output/formatcanon.h"
#include "playback/output/framehandle.h"
#include "playback/output/framepixelformat.h"

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

QTEST_GUILESS_MAIN(TestFormatCanon)
#include "tst_formatcanon.moc"
