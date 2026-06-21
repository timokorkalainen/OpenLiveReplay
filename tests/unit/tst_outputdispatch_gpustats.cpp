#include <QtTest>

#include "playback/output/gpureadbacktelemetry.h"
#include "playback/output/outputdispatcher.h"

static FrameHandle video(int feed, qint64 pts, uchar y) {
    FrameHandle handle = solidYuv420pHandle(4, 4, y, 128, 128);
    handle.metadata().key.feedIndex = feed;
    handle.metadata().key.ptsMs = pts;
    return handle;
}

class CollectingSink final : public IOutputSink {
public:
    explicit CollectingSink(OutputTargetKind kind) : m_kind(kind) {}

    OutputTargetKind kind() const override { return m_kind; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }

    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame&) override { return m_active; }

private:
    OutputTargetKind m_kind = OutputTargetKind::QtPreview;
    bool m_active = false;
};

class TestOutputDispatchGpuStats : public QObject {
    Q_OBJECT
private slots:
    void init() { GpuReadbackTelemetry::instance().reset(); }

    void newFieldsDefaultZero();
    void cpuPathLeavesGpuCountersZero();
    void redundantReadbackSurfacesThroughStats();
};

void TestOutputDispatchGpuStats::newFieldsDefaultZero() {
    OutputDispatchStats stats;
    QCOMPARE(stats.gpuVramBytes, qint64(0));
    QCOMPARE(stats.readbackQueueDepth, qint64(0));
    QCOMPARE(stats.readbackDrops, qint64(0));
    QCOMPARE(stats.fenceWaitStalls, qint64(0));
    QCOMPARE(stats.gpuOomDegrades, qint64(0));
    QCOMPARE(stats.gpuReadbacks, qint64(0));
    QCOMPARE(stats.redundantGpuReadbacks, qint64(0));
}

void TestOutputDispatchGpuStats::cpuPathLeavesGpuCountersZero() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{feed0, &sink}});

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.selectedFeedIndex = 0;

    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 77));
    for (int i = 0; i < 5; ++i)
        dispatcher.dispatchTick(cache, state);

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.gpuReadbacks, qint64(0));
    QCOMPARE(stats.redundantGpuReadbacks, qint64(0));
    QCOMPARE(stats.gpuVramBytes, qint64(0));
    QCOMPARE(stats.readbackQueueDepth, qint64(0));
    QCOMPARE(stats.readbackDrops, qint64(0));
    QCOMPARE(stats.fenceWaitStalls, qint64(0));
    QCOMPARE(stats.gpuOomDegrades, qint64(0));
}

void TestOutputDispatchGpuStats::redundantReadbackSurfacesThroughStats() {
    OutputTargetAssignment feed0;
    feed0.id = QStringLiteral("feed0-preview");
    feed0.sourceBus = OutputBusId::feed(0);
    feed0.kind = OutputTargetKind::QtPreview;
    feed0.enabled = true;

    CollectingSink sink(OutputTargetKind::QtPreview);
    OutputDispatcher dispatcher(FrameRate::fromFraction(25, 1), 1, 4, 4);
    dispatcher.setEndpoints({{feed0, &sink}});

    const GpuReadbackSurfaceKey key{0u, 3, FramePixelFormat::Yuv420p};
    GpuReadbackTelemetry::instance().recordSurface(key);
    GpuReadbackTelemetry::instance().recordGpuReadback(key);
    GpuReadbackTelemetry::instance().recordGpuReadback(key);

    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = true;
    state.selectedFeedIndex = 0;
    OutputFrameCache cache(1, 4, 4);
    cache.insertVideoFrame(video(0, 100, 77));
    dispatcher.dispatchTick(cache, state);

    const OutputDispatchStats stats = dispatcher.stats();
    QCOMPARE(stats.gpuReadbacks, qint64(2));
    QCOMPARE(stats.redundantGpuReadbacks, qint64(1));
}

QTEST_GUILESS_MAIN(TestOutputDispatchGpuStats)
#include "tst_outputdispatch_gpustats.moc"
