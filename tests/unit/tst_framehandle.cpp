#include <QtTest>

#include "playback/output/colormetadata.h"
#include "playback/output/framehandle.h"
#include "playback/output/framepixelformat.h"
#include "playback/output/outputtypes.h"

class TestFrameHandle : public QObject {
    Q_OBJECT
private slots:
    void planeCountPerFormat();
    void colorMetadataDefaultsAndEquality();
    void payloadKeyExcludesPresentationFields();
    void frameMetadataCarriesPresentationAndColor();
    void cpuFrameHandleReadToCpuYuv420pIsPassthrough();
    void copyIsRefcountBumpNotPixelCopy();
    void aliasedHandleMetadataOverrideKeepsPixelsByteIdentical();
    void solidHelperMatchesMediaFrameLayout();
    void cpuHandleIsNotGpuBackedAndHasNoSurface();
    void mediaVideoFrameViewMirrorsLegacyFields();
    void yuv420pOrdinalMatchesLegacyForHashStability();
};

void TestFrameHandle::planeCountPerFormat() {
    QCOMPARE(planeCount(FramePixelFormat::Nv12), 2);
    QCOMPARE(planeCount(FramePixelFormat::Yuv420p), 3);
    QCOMPARE(planeCount(FramePixelFormat::Rgba8), 1);
}

void TestFrameHandle::colorMetadataDefaultsAndEquality() {
    ColorMetadata a;
    QCOMPARE(a.matrix, ColorMatrix::Bt709);
    QCOMPARE(a.range, ColorRange::Video);
    QCOMPARE(a.primaries, ColorPrimaries::Bt709);
    QCOMPARE(a.transfer, ColorTransfer::Bt709);
    QCOMPARE(a.chromaFormat, ChromaFormat::Yuv420);
    QCOMPARE(a.bitDepth, 8);

    ColorMetadata b;
    QVERIFY(a == b);
    b.matrix = ColorMatrix::Bt601;
    QVERIFY(a != b);
}

void TestFrameHandle::payloadKeyExcludesPresentationFields() {
    FramePayloadKey a;
    a.feedIndex = 1;
    a.ptsMs = 100;
    a.videoHash = 0xABCD;
    a.format = FramePixelFormat::Yuv420p;
    a.width = 4;
    a.height = 4;
    a.isPlaceholder = false;

    FramePayloadKey b = a;
    QVERIFY(a.samePayloadAs(b));

    b.ptsMs = 101;
    QVERIFY(!a.samePayloadAs(b));
    b = a;
    b.videoHash = 0x1234;
    QVERIFY(!a.samePayloadAs(b));
    b = a;
    b.feedIndex = 2;
    QVERIFY(!a.samePayloadAs(b));
}

void TestFrameHandle::frameMetadataCarriesPresentationAndColor() {
    FrameMetadata m;
    m.key.feedIndex = 3;
    m.key.ptsMs = 200;
    m.key.format = FramePixelFormat::Yuv420p;
    m.key.width = 8;
    m.key.height = 8;
    m.outputFrameIndex = 42;
    m.sampledPlayheadMs = 333;
    m.stride[0] = 8;
    m.stride[1] = 4;
    m.stride[2] = 4;
    m.color.matrix = ColorMatrix::Bt601;

    QCOMPARE(m.key.feedIndex, 3);
    QCOMPARE(m.outputFrameIndex, qint64(42));
    QCOMPARE(m.sampledPlayheadMs, qint64(333));
    QCOMPARE(m.stride[1], 4);
    QCOMPARE(m.color.matrix, ColorMatrix::Bt601);

    FrameMetadata n = m;
    n.outputFrameIndex = 99;
    n.sampledPlayheadMs = 777;
    QVERIFY(m.key.samePayloadAs(n.key));
}

void TestFrameHandle::cpuFrameHandleReadToCpuYuv420pIsPassthrough() {
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = 4;
    p.height = 4;
    p.stride[0] = 4;
    p.stride[1] = 2;
    p.stride[2] = 2;
    p.plane[0] = QByteArray(16, char(10));
    p.plane[1] = QByteArray(4, char(128));
    p.plane[2] = QByteArray(4, char(128));

    FrameMetadata m;
    m.key.format = FramePixelFormat::Yuv420p;
    m.key.width = 4;
    m.key.height = 4;
    FrameHandle h = makeCpuFrameHandle(p, m);

    const CpuPlanes out = h.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(out.format, FramePixelFormat::Yuv420p);
    QCOMPARE(out.width, 4);
    QCOMPARE(out.plane[0], p.plane[0]);
    QCOMPARE(out.plane[1], p.plane[1]);
    QCOMPARE(out.plane[2], p.plane[2]);
    QVERIFY(out.plane[0].constData() == p.plane[0].constData());
}

void TestFrameHandle::copyIsRefcountBumpNotPixelCopy() {
    FrameHandle a = solidYuv420pHandle(4, 4, 16, 128, 128);
    const long before = a.dataPtr().use_count();
    FrameHandle b = a;
    QCOMPARE(b.dataPtr().use_count(), before + 1);
    QVERIFY(a.dataPtr().get() == b.dataPtr().get());
}

void TestFrameHandle::aliasedHandleMetadataOverrideKeepsPixelsByteIdentical() {
    FrameHandle original = solidYuv420pHandle(4, 4, 70, 90, 110);
    original.metadata().key.ptsMs = 100;
    original.metadata().outputFrameIndex = 5;

    const CpuPlanes originalPixels = original.readToCpu(FramePixelFormat::Yuv420p);
    const long originalUse = original.dataPtr().use_count();

    FrameHandle aliased = original;
    aliased.metadata().key.ptsMs = 999;
    aliased.metadata().outputFrameIndex = 42;

    const CpuPlanes aliasedPixels = aliased.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(aliasedPixels.plane[0], originalPixels.plane[0]);
    QCOMPARE(aliasedPixels.plane[1], originalPixels.plane[1]);
    QCOMPARE(aliasedPixels.plane[2], originalPixels.plane[2]);
    QVERIFY(aliased.dataPtr().get() == original.dataPtr().get());
    QCOMPARE(aliased.dataPtr().use_count(), originalUse + 1);

    QCOMPARE(original.metadata().key.ptsMs, qint64(100));
    QCOMPARE(original.metadata().outputFrameIndex, qint64(5));
}

void TestFrameHandle::solidHelperMatchesMediaFrameLayout() {
    FrameHandle h = solidYuv420pHandle(4, 4, 16, 128, 128);
    const CpuPlanes p = h.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(p.width, 4);
    QCOMPARE(p.height, 4);
    QCOMPARE(p.stride[0], 4);
    QCOMPARE(p.stride[1], 2);
    QCOMPARE(p.stride[2], 2);
    QCOMPARE(p.plane[0], QByteArray(16, char(16)));
    QCOMPARE(p.plane[1], QByteArray(4, char(128)));
    QCOMPARE(p.plane[2], QByteArray(4, char(128)));
}

void TestFrameHandle::cpuHandleIsNotGpuBackedAndHasNoSurface() {
    FrameHandle h = solidYuv420pHandle(4, 4, 16, 128, 128);
    QVERIFY(!h.isGpuBacked());
    QVERIFY(h.data()->isCpuBacked());
    QVERIFY(h.data()->gpuSurface() == nullptr);
}

void TestFrameHandle::mediaVideoFrameViewMirrorsLegacyFields() {
    FrameHandle h = solidYuv420pHandle(4, 4, 70, 90, 110);
    h.metadata().key.feedIndex = 2;
    h.metadata().key.ptsMs = 123;
    h.metadata().outputFrameIndex = 7;
    h.metadata().key.isPlaceholder = false;

    MediaVideoFrameView v(h);
    QCOMPARE(v.feedIndex, 2);
    QCOMPARE(v.ptsMs, qint64(123));
    QCOMPARE(v.outputFrameIndex, qint64(7));
    QCOMPARE(v.width, 4);
    QCOMPARE(v.height, 4);
    QVERIFY(!v.isPlaceholder);
    QCOMPARE(v.strideY, 4);
    QCOMPARE(v.strideU, 2);
    QCOMPARE(v.strideV, 2);
    QCOMPARE(uchar(v.planeY.at(0)), uchar(70));
    QCOMPARE(uchar(v.planeU.at(0)), uchar(90));
    QCOMPARE(uchar(v.planeV.at(0)), uchar(110));
    QVERIFY(v.isValid());

    FrameHandle empty;
    MediaVideoFrameView ev(empty);
    QVERIFY(!ev.isValid());
}

void TestFrameHandle::yuv420pOrdinalMatchesLegacyForHashStability() {
    QCOMPARE(int(FramePixelFormat::Yuv420p), int(MediaPixelFormat::Yuv420p));
}

QTEST_GUILESS_MAIN(TestFrameHandle)
#include "tst_framehandle.moc"
