// DeckLinkOutputSink mirrors the NDI sink seam: a neutral shell over an
// injectable backend. Off-SDK it reports RuntimeUnavailable; an injected fake
// exercises GPU-native vs CPU-frame routing and resolved capability.
#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/framehandle.h"
#include "playback/output/iotargets/decklinksink.h"
#include "playback/output/outputbusengine.h"

#include <utility>

namespace {

OutputBusFrame solidBusFrame() {
    OutputBusFrame frame;
    frame.bus = OutputBusId::pgm();
    frame.outputFrameIndex = 7;
    frame.video = solidYuv420pHandle(64, 48, 128, 128, 128);
    return frame;
}

class FakeGpuFrameData final : public IFrameData {
public:
    explicit FakeGpuFrameData(std::shared_ptr<GpuSurface> surface = nullptr,
                              std::shared_ptr<GpuFence> fence = nullptr)
        : m_surface(std::move(surface)), m_fence(std::move(fence)) {}

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat) const override { return CpuPlanes{}; }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_fence; }
    GpuFrameSynchronization gpuSynchronization() const override {
        const uint64_t value = m_surface ? m_surface->pendingFenceValue() : 0;
        if (value == 0) return {};
        return {m_fence, value, true};
    }
    FramePixelFormat nativeFormat() const override { return FramePixelFormat::Nv12; }

private:
    std::shared_ptr<GpuSurface> m_surface;
    std::shared_ptr<GpuFence> m_fence;
};

class FakeGpuSurface final : public GpuSurface {
public:
    explicit FakeGpuSurface(bool valid = true,
                            void* handle = reinterpret_cast<void*>(quintptr(0x1)),
                            uint64_t pendingFence = 0)
        : m_valid(valid), m_handle(handle), m_pendingFence(pendingFence) {}

    GpuSurfaceDesc desc() const override {
        return {.format = FramePixelFormat::Nv12, .width = 64, .height = 48};
    }
    bool isValid() const override { return m_valid; }
    void* nativeHandle() const override { return m_handle; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    bool m_valid = true;
    void* m_handle = nullptr;
    uint64_t m_pendingFence = 0;
};

class ManualFence final : public GpuFence {
public:
    uint64_t signal() override { return 1; }
    bool wait(uint64_t value, int timeoutMs) override {
        ++waits;
        lastValue = value;
        lastTimeoutMs = timeoutMs;
        return completedValue() >= value;
    }
    uint64_t completedValue() const override { return completed; }

    uint64_t completed = 0;
    uint64_t lastValue = 0;
    int lastTimeoutMs = 0;
    int waits = 0;
};

std::shared_ptr<GpuFence> readyFence() {
    auto fence = std::make_shared<ManualFence>();
    fence->completed = 1;
    return fence;
}

OutputBusFrame gpuBusFrame(std::shared_ptr<GpuSurface> surface,
                           std::shared_ptr<GpuFence> fence = nullptr) {
    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Nv12;
    meta.key.width = 64;
    meta.key.height = 48;

    OutputBusFrame frame;
    frame.bus = OutputBusId::pgm();
    frame.outputFrameIndex = 8;
    frame.video =
        FrameHandle(std::make_shared<FakeGpuFrameData>(std::move(surface), std::move(fence)), meta);
    return frame;
}

OutputBusFrame gpuBusFrameWithInvalidMetadata() {
    OutputBusFrame frame = gpuBusFrame(
        std::make_shared<FakeGpuSurface>(true, reinterpret_cast<void*>(quintptr(0x1)), 1),
        readyFence());
    frame.video.metadata().key.width = 0;
    return frame;
}

OutputBusFrame nullSurfaceGpuBusFrame() {
    return gpuBusFrame(nullptr);
}

OutputBusFrame presentableGpuBusFrame() {
    return gpuBusFrame(
        std::make_shared<FakeGpuSurface>(true, reinterpret_cast<void*>(quintptr(0x1)), 1),
        readyFence());
}

OutputBusFrame unfencedPresentableGpuBusFrame() {
    return gpuBusFrame(std::make_shared<FakeGpuSurface>());
}

OutputBusFrame pendingGpuBusFrame(std::shared_ptr<GpuFence> fence, uint64_t value) {
    return gpuBusFrame(
        std::make_shared<FakeGpuSurface>(true, reinterpret_cast<void*>(quintptr(0x1)), value),
        std::move(fence));
}

OutputBusFrame invalidSurfaceGpuBusFrame() {
    return gpuBusFrame(
        std::make_shared<FakeGpuSurface>(false, reinterpret_cast<void*>(quintptr(0x1))));
}

OutputBusFrame invalidNativeGpuBusFrame() {
    return gpuBusFrame(std::make_shared<FakeGpuSurface>(true, nullptr));
}

class FakeDeckLinkBackend final : public IDeckLinkSenderBackend {
public:
    bool available = true;
    bool gpuTextureInput = false;
    int cpuScheduled = 0;
    int gpuScheduled = 0;
    int st2110CpuScheduled = 0;
    int st2110GpuScheduled = 0;
    St2110VideoFrame lastSt2110Frame;
    OutputBusFrame retainedGpuFrame;

    bool isRuntimeAvailable() const override { return available; }
    bool deviceSupportsGpuTextureInput() const override { return gpuTextureInput; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return available; }
    void closeDevice() override {}
    bool scheduleFrame(OutputBusFrame) override {
        ++cpuScheduled;
        return true;
    }
    bool scheduleGpuFrame(OutputBusFrame frame) override {
        ++gpuScheduled;
        retainedGpuFrame = std::move(frame);
        return true;
    }
    bool scheduleSt2110Frame(OutputBusFrame, St2110VideoFrame st2110Frame) override {
        ++st2110CpuScheduled;
        lastSt2110Frame = std::move(st2110Frame);
        return true;
    }
    bool scheduleGpuSt2110Frame(OutputBusFrame frame, St2110VideoFrame st2110Frame) override {
        ++st2110GpuScheduled;
        retainedGpuFrame = std::move(frame);
        lastSt2110Frame = std::move(st2110Frame);
        return true;
    }
};

OutputTargetAssignment sdiAssignment() {
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::DeckLinkSdiHdmi;
    assignment.enabled = true;
    return assignment;
}

} // namespace

class TestDeckLinkSink : public QObject {
    Q_OBJECT
private slots:
    void stubBackendReportsRuntimeUnavailable();
    void defaultBackendUnavailableOffSdk();
    void gpuDeviceResolvesGpuNativeAndSubmitsTexture();
    void gpuNativeWaitsForProducerFenceBeforeSubmit();
    void gpuNativeBackendCanRetainSubmittedFrame();
    void gpuNativeRejectsUnfencedNativeSurface();
    void gpuNativeRejectsReadyFenceWithoutSurfaceFence();
    void gpuNativeRejectsPendingFrameWithoutFence();
    void gpuNativeRejectsMissingSurface();
    void gpuNativeRejectsInvalidMetadata();
    void gpuNativeRejectsInvalidSurface();
    void gpuNativeRejectsInvalidNativeSurface();
    void gpuNativeRejectsCpuFrame();
    void cpuDeviceResolvesCadenceAndSchedulesCpuFrame();
    void st2110CpuDeviceUsesSt2110Framer();
    void st2110GpuDeviceUsesGpuSt2110Framer();
    void gpuNativeStillRequiresContinuousCadence();
};

void TestDeckLinkSink::stubBackendReportsRuntimeUnavailable() {
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi);
    QVERIFY(!sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QVERIFY(!sink.isActive());
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}

void TestDeckLinkSink::defaultBackendUnavailableOffSdk() {
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkIpSt2110);
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::DeckLinkIpSt2110;
    assignment.enabled = true;

    QVERIFY(!sink.start(assignment, FrameRate::fromFraction(50, 1)));
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}

void TestDeckLinkSink::gpuDeviceResolvesGpuNativeAndSubmitsTexture() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(sink.submit(presentableGpuBusFrame()));
    QCOMPARE(backend.gpuScheduled, 1);
    QCOMPARE(backend.cpuScheduled, 0);
}

void TestDeckLinkSink::gpuNativeWaitsForProducerFenceBeforeSubmit() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);
    auto fence = std::make_shared<ManualFence>();
    fence->completed = 7;

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QVERIFY(sink.submit(pendingGpuBusFrame(fence, 7)));
    QCOMPARE(fence->waits, 1);
    QCOMPARE(fence->lastValue, uint64_t(7));
    QVERIFY(fence->lastTimeoutMs > 0);
    QCOMPARE(backend.gpuScheduled, 1);
    QCOMPARE(backend.cpuScheduled, 0);
}

void TestDeckLinkSink::gpuNativeBackendCanRetainSubmittedFrame() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);
    auto surface =
        std::make_shared<FakeGpuSurface>(true, reinterpret_cast<void*>(quintptr(0x1)), 1);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    {
        OutputBusFrame frame = gpuBusFrame(surface, readyFence());
        QVERIFY(sink.submit(frame));
    }
    surface.reset();

    QCOMPARE(backend.gpuScheduled, 1);
    QVERIFY(backend.retainedGpuFrame.video.isGpuBacked());
    QVERIFY(backend.retainedGpuFrame.video.data());
    QVERIFY(backend.retainedGpuFrame.video.data()->gpuSurface() != nullptr);
}

void TestDeckLinkSink::gpuNativeRejectsUnfencedNativeSurface() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QVERIFY(!sink.submit(unfencedPresentableGpuBusFrame()));
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsReadyFenceWithoutSurfaceFence() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QVERIFY(!sink.submit(gpuBusFrame(std::make_shared<FakeGpuSurface>(), readyFence())));
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsPendingFrameWithoutFence() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QVERIFY(!sink.submit(pendingGpuBusFrame(nullptr, 7)));
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsMissingSurface() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(!sink.submit(nullSurfaceGpuBusFrame()));
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsInvalidMetadata() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(!sink.submit(gpuBusFrameWithInvalidMetadata()));
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsInvalidSurface() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(!sink.submit(invalidSurfaceGpuBusFrame()));
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsInvalidNativeSurface() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(!sink.submit(invalidNativeGpuBusFrame()));
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::gpuNativeRejectsCpuFrame() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(!sink.submit(solidBusFrame()));
    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(sink.outputStatus().state, QStringLiteral("send-failed"));
}

void TestDeckLinkSink::cpuDeviceResolvesCadenceAndSchedulesCpuFrame() {
    FakeDeckLinkBackend backend;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::NeedsContinuousCadence);
    QVERIFY(sink.submit(solidBusFrame()));
    QCOMPARE(backend.cpuScheduled, 1);
    QCOMPARE(backend.gpuScheduled, 0);
}

void TestDeckLinkSink::st2110CpuDeviceUsesSt2110Framer() {
    FakeDeckLinkBackend backend;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkIpSt2110, &backend);
    OutputTargetAssignment assignment = sdiAssignment();
    assignment.kind = OutputTargetKind::DeckLinkIpSt2110;
    OutputBusFrame frame = solidBusFrame();
    frame.outputFrameIndex = 2;
    frame.programmeTimecode100ns = 10000000;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(60, 1)));
    QVERIFY(sink.submit(frame));

    const CpuPlanes planes = frame.video.readToCpu(FramePixelFormat::Yuv420p);
    QByteArray expectedEssence;
    expectedEssence.reserve(planes.plane[0].size() + planes.plane[1].size() +
                            planes.plane[2].size());
    expectedEssence.append(planes.plane[0]);
    expectedEssence.append(planes.plane[1]);
    expectedEssence.append(planes.plane[2]);

    QCOMPARE(backend.cpuScheduled, 0);
    QCOMPARE(backend.st2110CpuScheduled, 1);
    QCOMPARE(backend.lastSt2110Frame.rtpTimestamp90k, quint32(90000));
    QCOMPARE(backend.lastSt2110Frame.payloadType, quint8(96));
    QCOMPARE(backend.lastSt2110Frame.ssrc, quint32(0x4f4c5231u));
    QVERIFY(backend.lastSt2110Frame.markerLast);
    QCOMPARE(backend.lastSt2110Frame.essence, expectedEssence);
}

void TestDeckLinkSink::st2110GpuDeviceUsesGpuSt2110Framer() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkIpSt2110, &backend);
    OutputTargetAssignment assignment = sdiAssignment();
    assignment.kind = OutputTargetKind::DeckLinkIpSt2110;
    OutputBusFrame frame = presentableGpuBusFrame();
    frame.outputFrameIndex = 2;
    frame.identity.videoHash = 12345;
    frame.video.metadata().gpuGeneration = 11;

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(60, 1)));
    QVERIFY(sink.submit(frame));

    QCOMPARE(backend.gpuScheduled, 0);
    QCOMPARE(backend.st2110GpuScheduled, 1);
    QCOMPARE(backend.lastSt2110Frame.rtpTimestamp90k, quint32(3000));
    QCOMPARE(backend.lastSt2110Frame.payloadType, quint8(96));
    QCOMPARE(backend.lastSt2110Frame.ssrc, quint32(0x4f4c5231u));
    QVERIFY(backend.lastSt2110Frame.markerLast);
    QCOMPARE(backend.lastSt2110Frame.essence, QByteArrayLiteral("2:12345:11"));
    QVERIFY(backend.retainedGpuFrame.video.isGpuBacked());
}

void TestDeckLinkSink::gpuNativeStillRequiresContinuousCadence() {
    FakeDeckLinkBackend backend;
    backend.gpuTextureInput = true;
    DeckLinkOutputSink sink(OutputTargetKind::DeckLinkSdiHdmi, &backend);

    QVERIFY(sink.start(sdiAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::GpuNative);
    QVERIFY(sink.needsContinuousCadence());
}

QTEST_GUILESS_MAIN(TestDeckLinkSink)
#include "tst_decklinksink.moc"
