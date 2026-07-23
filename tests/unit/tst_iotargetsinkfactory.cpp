// makeIoTargetSink owns the new broadcast I/O target construction path and
// applies D10 readback routing. Off-SDK backends are unavailable, but the
// factory still returns inactive sinks with the correct kind.
#include <QtTest>

#include "playback/output/asyncgpureadbacksink.h"
#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "playback/output/iotargets/decklinksink.h"
#include "playback/output/iotargets/iotargetsinkfactory.h"
#include "playback/output/outputtargetassignment.h"

#include <atomic>

namespace {

constexpr uintptr_t kTestDeviceDomainId = 0x1234u;

class FakeDeckLinkBackend final : public IDeckLinkSenderBackend {
public:
    bool available = true;
    bool gpuTextureInput = false;
    std::atomic<int> opened{0};
    std::atomic<int> cpuScheduled{0};
    std::atomic<int> gpuScheduled{0};

    bool isRuntimeAvailable() const override { return available; }
    bool deviceSupportsGpuTextureInput() const override {
        return opened.load(std::memory_order_acquire) > 0 && gpuTextureInput;
    }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override {
        if (!available) return false;
        opened.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    void closeDevice() override {}
    bool scheduleFrame(OutputBusFrame) override {
        cpuScheduled.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    bool scheduleGpuFrame(OutputBusFrame) override {
        gpuScheduled.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
};

class FakeGpuSurface final : public GpuSurface {
public:
    FakeGpuSurface()
        : m_authorityEpoch(GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch()) {
        retainUntilFenceRetired(1);
    }
    GpuSurfaceDesc desc() const override {
        return {.format = FramePixelFormat::Nv12, .width = 64, .height = 48};
    }
    bool isValid() const override { return true; }
    GpuSurfaceCompatibility compatibility() const override {
        return {kTestDeviceDomainId, m_authorityEpoch};
    }
    void* nativeHandle() const override { return reinterpret_cast<void*>(quintptr(0x1)); }

private:
    uint64_t m_authorityEpoch = 0;
};

class ReadyFence final : public GpuFence {
public:
    ReadyFence()
        : GpuFence(kTestDeviceDomainId,
                   GpuDeviceLossMonitor::instance().currentDeviceAuthorityEpoch()) {}

    uint64_t signal() override { return 1; }
    bool wait(uint64_t value, int) override { return value <= 1; }
    uint64_t completedValue() const override { return 1; }
};

class FakeGpuFrameData final : public IFrameData {
public:
    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override {
        readCount.fetch_add(1, std::memory_order_acq_rel);
        return solidYuv420pHandle(64, 48, 72, 128, 128).readToCpu(target);
    }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_fence; }
    GpuFrameSynchronization gpuSynchronization() const override { return {m_fence, 1, true}; }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }
    int readbackCount() const { return readCount.load(std::memory_order_acquire); }

private:
    std::shared_ptr<GpuSurface> m_surface = std::make_shared<FakeGpuSurface>();
    std::shared_ptr<GpuFence> m_fence = std::make_shared<ReadyFence>();
    mutable std::atomic<int> readCount{0};
};

OutputTargetAssignment deckLinkAssignment() {
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::DeckLinkSdiHdmi;
    assignment.enabled = true;
    return assignment;
}

OutputBusFrame presentableGpuFrame(
    const std::shared_ptr<FakeGpuFrameData>& data = std::make_shared<FakeGpuFrameData>()) {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;
    meta.gpuGeneration = GpuGenerationCounter::instance().current();

    OutputBusFrame frame;
    frame.bus = OutputBusId::pgm();
    frame.video = FrameHandle(data, meta);
    return frame;
}

} // namespace

class TestIoTargetSinkFactory : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void returnsSinkPerKind_data();
    void returnsSinkPerKind();
    void deckLinkGpuCapabilityIsResolvedAfterStart();
    void deckLinkCpuFallbackUsesReadbackWrapperAfterStart();
    void usesDepthThreeForNonPgmCpuTargets();
    void returnsNullForUnownedKind();
};

void TestIoTargetSinkFactory::cleanup() {
    qunsetenv("OLR_GPU_PIPELINE");
}

void TestIoTargetSinkFactory::returnsSinkPerKind_data() {
    QTest::addColumn<int>("kind");

    QTest::newRow("decklink-sdi") << int(OutputTargetKind::DeckLinkSdiHdmi);
    QTest::newRow("decklink-st2110") << int(OutputTargetKind::DeckLinkIpSt2110);
    QTest::newRow("aja") << int(OutputTargetKind::Aja);
    QTest::newRow("omt") << int(OutputTargetKind::Omt);
}

void TestIoTargetSinkFactory::returnsSinkPerKind() {
    QFETCH(int, kind);
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind(kind);
    assignment.enabled = true;

    auto sink = makeIoTargetSink(assignment, FrameRate::fromFraction(60, 1));
    QVERIFY(sink != nullptr);
    QCOMPARE(sink->kind(), OutputTargetKind(kind));
    QVERIFY(!sink->isActive());

    auto* readback = dynamic_cast<AsyncGpuReadbackSink*>(sink.get());
    const auto targetKind = OutputTargetKind(kind);
    if (targetKind == OutputTargetKind::DeckLinkSdiHdmi ||
        targetKind == OutputTargetKind::DeckLinkIpSt2110) {
        QVERIFY(readback == nullptr);
    } else {
        QVERIFY(readback != nullptr);
        QCOMPARE(readback->ringDepth(), 1);
    }
}

void TestIoTargetSinkFactory::deckLinkGpuCapabilityIsResolvedAfterStart() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;

    auto sink = makeDeckLinkIoTargetSinkForTest(OutputTargetKind::DeckLinkSdiHdmi, &backend);
    QVERIFY(sink != nullptr);
    QVERIFY(sink->start(deckLinkAssignment(), FrameRate::fromFraction(60, 1)));

    qint64 depth = 0;
    qint64 drops = 0;
    QVERIFY(!sink->readbackStats(depth, drops));
    QVERIFY(sink->submit(presentableGpuFrame()));
    QCOMPARE(backend.opened.load(std::memory_order_acquire), 1);
    QCOMPARE(backend.gpuScheduled.load(std::memory_order_acquire), 1);
    QCOMPARE(backend.cpuScheduled.load(std::memory_order_acquire), 0);
}

void TestIoTargetSinkFactory::deckLinkCpuFallbackUsesReadbackWrapperAfterStart() {
    qputenv("OLR_GPU_PIPELINE", "1");
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = false;
    const auto data = std::make_shared<FakeGpuFrameData>();

    auto sink = makeDeckLinkIoTargetSinkForTest(OutputTargetKind::DeckLinkSdiHdmi, &backend);
    QVERIFY(sink != nullptr);
    QVERIFY(sink->start(deckLinkAssignment(), FrameRate::fromFraction(60, 1)));

    qint64 depth = -1;
    qint64 drops = -1;
    QVERIFY(sink->readbackStats(depth, drops));
    QCOMPARE(depth, qint64(0));
    QCOMPARE(drops, qint64(0));
    QVERIFY(sink->isActive());
    QCOMPARE(backend.opened.load(std::memory_order_acquire), 1);
    QVERIFY(sink->submit(presentableGpuFrame(data)));
    QTRY_COMPARE_WITH_TIMEOUT(backend.cpuScheduled.load(std::memory_order_acquire), 1, 1000);
    QCOMPARE(data->readbackCount(), 1);
    QCOMPARE(backend.gpuScheduled.load(std::memory_order_acquire), 0);
}

void TestIoTargetSinkFactory::usesDepthThreeForNonPgmCpuTargets() {
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Aja;
    assignment.enabled = true;
    assignment.sourceBus = OutputBusId::feed(0);

    auto sink = makeIoTargetSink(assignment, FrameRate::fromFraction(50, 1));
    auto* readback = dynamic_cast<AsyncGpuReadbackSink*>(sink.get());
    QVERIFY(readback != nullptr);
    QCOMPARE(readback->ringDepth(), 3);
}

void TestIoTargetSinkFactory::returnsNullForUnownedKind() {
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Ndi;
    assignment.enabled = true;

    QVERIFY(makeIoTargetSink(assignment, FrameRate::fromFraction(60, 1)) == nullptr);
}

QTEST_GUILESS_MAIN(TestIoTargetSinkFactory)
#include "tst_iotargetsinkfactory.moc"
