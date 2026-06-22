// GpuCompositor is the RHI grid/PGM compositor; the CPU Yuv420pCompositor stays
// the oracle + fallback. With no RHI backend create() may return nullptr; with a
// backend it must never return a partially initialized compositor.
#include <QtTest>

#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/framehandle.h"

class TestGpuCompositor : public QObject {
    Q_OBJECT
private slots:
    void createIsNullOrValidNeverPartial();
};

void TestGpuCompositor::createIsNullOrValidNeverPartial() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend on this host");

    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor pipeline unavailable on this host");

    QVERIFY(comp->isValid());
}

QTEST_GUILESS_MAIN(TestGpuCompositor)
#include "tst_gpucompositor.moc"
