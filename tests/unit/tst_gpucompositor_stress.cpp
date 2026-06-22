// Multi-feed cap-pressure GPU compositor stress (spec §7 Phase 3): concurrent
// compose-while-evict, stale-generation drop, and OOM-degrade-to-CPU. TSan cannot
// see GPU command ordering, so these are first-class gates. Each SKIPs with no RHI.
#include <QtTest>

#include "playback/gpu/gpucompositor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpupipelineconfig.h"
#include "playback/gpu/gpurhicontext.h"
#include "playback/output/formatcanon.h"
#include "playback/output/framehandle.h"
#include "playback/output/yuv420pcompositor.h"

#include <atomic>
#include <thread>

class TestGpuCompositorStress : public QObject {
    Q_OBJECT
private slots:
    void staleGenerationInputIsDroppedAsAbsent();
    void oomDegradesToCpuWithoutCrashing();
    void concurrentComposeAndChecksumValidate();
};

namespace {

class ScopedInjectedAllocFailures {
public:
    explicit ScopedInjectedAllocFailures(int count) { gpuSetInjectedAllocFailures(count); }
    ~ScopedInjectedAllocFailures() { gpuSetInjectedAllocFailures(0); }
};

int maxChannelDelta(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return 256;
    int maxDelta = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        maxDelta = qMax(maxDelta, qAbs(int(uchar(a.at(i))) - int(uchar(b.at(i)))));
    }
    return maxDelta;
}

} // namespace

void TestGpuCompositorStress::staleGenerationInputIsDroppedAsAbsent() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    GpuGenerationCounter::instance().resetForTest();
    const uint64_t generation = GpuGenerationCounter::instance().bump();
    FrameHandle stale = solidYuv420pHandle(4, 4, 200, 128, 128);
    stale.metadata().gpuGeneration = generation;
    GpuGenerationCounter::instance().bump();

    ColorMetadata color;
    const CpuPlanes got =
        comp->composeGridToCpu({stale}, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    if (!got.isValid()) QSKIP("RGBA readback unavailable");

    const CpuPlanes bgOnly =
        formatcanon::referenceComposeGridRgba8(QList<FrameHandle>{}, 8, 8, color);
    QCOMPARE(got.plane[0], bgOnly.plane[0]);
}

void TestGpuCompositorStress::oomDegradesToCpuWithoutCrashing() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200)};
    ColorMetadata color;
    ScopedInjectedAllocFailures injectOne(1);
    const FrameHandle gpu =
        comp->composeGrid(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
    QVERIFY(gpu.isNull());

    const FrameHandle cpu = Yuv420pCompositor::composeGrid(frames, 8, 8);
    QVERIFY(!cpu.isNull());
}

void TestGpuCompositorStress::concurrentComposeAndChecksumValidate() {
    auto rhi = GpuRhiContext::create();
    if (!rhi) QSKIP("no RHI backend");
    auto comp = GpuCompositor::create(rhi);
    if (!comp) QSKIP("compositor unavailable");

    GpuGenerationCounter::instance().resetForTest();
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200),
                              solidYuv420pHandle(4, 4, 160, 90, 170)};
    ColorMetadata color;
    const CpuPlanes oracle = formatcanon::referenceComposeGridRgba8(frames, 8, 8, color);
    QVERIFY(oracle.isValid());

    std::atomic<bool> stop{false};
    std::thread bumper([&] {
        while (!stop.load(std::memory_order_acquire)) {
            GpuGenerationCounter::instance().bump();
        }
    });

    int validFrames = 0;
    int worstDelta = 0;
    qsizetype wrongSize = -1;
    for (int i = 0; i < 50; ++i) {
        const CpuPlanes got =
            comp->composeGridToCpu(frames, 8, 8, color, GpuCompositor::ScaleQuality::NearestCompat);
        if (!got.isValid()) continue;

        ++validFrames;
        if (got.plane[0].size() != oracle.plane[0].size()) {
            wrongSize = got.plane[0].size();
            continue;
        }
        worstDelta = qMax(worstDelta, maxChannelDelta(got.plane[0], oracle.plane[0]));
    }

    stop.store(true, std::memory_order_release);
    bumper.join();

    QVERIFY2(validFrames > 0, "stress produced no successful compositor readbacks");
    QCOMPARE(wrongSize, qsizetype(-1));
    QVERIFY2(worstDelta <= 1,
             qPrintable(QStringLiteral("max channel delta %1 > 1 LSB").arg(worstDelta)));
}

QTEST_MAIN(TestGpuCompositorStress)
#include "tst_gpucompositor_stress.moc"
