// GpuSurface is the concrete type behind the Phase-1 opaque forward declaration.
// The shared header must stay platform-neutral, so this test compiles it without
// Apple, Metal, or D3D headers and exercises the desc/validity contract.
#include <QtTest>

#include "playback/gpu/gpusurface.h"
#include "playback/output/framepixelformat.h"

namespace {
class FakeGpuSurface : public GpuSurface {
public:
    FakeGpuSurface(GpuSurfaceDesc desc, bool valid) : m_desc(desc), m_valid(valid) {}

    GpuSurfaceDesc desc() const override { return m_desc; }
    bool isValid() const override { return m_valid; }
    void* nativeHandle() const override {
        return m_valid ? const_cast<FakeGpuSurface*>(this) : nullptr;
    }

private:
    GpuSurfaceDesc m_desc;
    bool m_valid = false;
};
} // namespace

class TestGpuSurface : public QObject {
    Q_OBJECT
private slots:
    void descRoundTrips();
    void invalidSurfaceHasNullHandle();
};

void TestGpuSurface::descRoundTrips() {
    FakeGpuSurface s({FramePixelFormat::Nv12, 1920, 1080}, true);
    QCOMPARE(s.desc().format, FramePixelFormat::Nv12);
    QCOMPARE(s.desc().width, 1920);
    QCOMPARE(s.desc().height, 1080);
    QVERIFY(s.isValid());
    QVERIFY(s.nativeHandle() != nullptr);
}

void TestGpuSurface::invalidSurfaceHasNullHandle() {
    FakeGpuSurface s({FramePixelFormat::Nv12, 0, 0}, false);
    QVERIFY(!s.isValid());
    QVERIFY(s.nativeHandle() == nullptr);
}

QTEST_GUILESS_MAIN(TestGpuSurface)
#include "tst_gpusurface.moc"
