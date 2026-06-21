#include <QtTest>

#include "playback/output/formatcanon.h"
#include "playback/output/framepixelformat.h"

class TestFormatCanon : public QObject {
    Q_OBJECT
private slots:
    void planeShapeMatchesYuv420pLayout();
    void planeShapeMatchesNv12Layout();
    void planeShapeMatchesRgba8Layout();
    void absentPlanesAreZero();
    void packedStrideIsWidthTimesBytesPerSample();
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

QTEST_GUILESS_MAIN(TestFormatCanon)
#include "tst_formatcanon.moc"
