// AjaOutputSink is a CPU-frame continuous-cadence sibling of NDI for NTV2
// AutoCirculate host buffers. Off-SDK it reports RuntimeUnavailable; an
// injected fake transfers the bus frame and resolved capability is constant.
#include <QtTest>

#include "playback/output/framehandle.h"
#include "playback/output/iotargets/ajasink.h"
#include "playback/output/outputbusengine.h"

namespace {

class FakeAjaBackend final : public IAjaSenderBackend {
public:
    bool available = true;
    int transferred = 0;

    bool isRuntimeAvailable() const override { return available; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return available; }
    void closeDevice() override {}
    bool transferFrame(const OutputBusFrame&) override {
        ++transferred;
        return true;
    }
};

OutputTargetAssignment ajaAssignment() {
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Aja;
    assignment.enabled = true;
    return assignment;
}

} // namespace

class TestAjaSink : public QObject {
    Q_OBJECT
private slots:
    void stubReportsRuntimeUnavailable();
    void capabilityIsContinuousCadence();
    void requestsContinuousCadence();
    void submitTransfersFrame();
};

void TestAjaSink::stubReportsRuntimeUnavailable() {
    AjaOutputSink sink;

    QVERIFY(!sink.start(ajaAssignment(), FrameRate::fromFraction(60, 1)));
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}

void TestAjaSink::capabilityIsContinuousCadence() {
    AjaOutputSink sink;

    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::NeedsContinuousCadence);
}

void TestAjaSink::requestsContinuousCadence() {
    AjaOutputSink sink;

    QVERIFY(sink.needsContinuousCadence());
}

void TestAjaSink::submitTransfersFrame() {
    FakeAjaBackend backend;
    AjaOutputSink sink(&backend);
    OutputBusFrame frame;
    frame.outputFrameIndex = 11;
    frame.video = solidYuv420pHandle(64, 48, 16, 128, 128);

    QVERIFY(sink.start(ajaAssignment(), FrameRate::fromFraction(60, 1)));
    QVERIFY(sink.submit(frame));
    QCOMPARE(backend.transferred, 1);
    QCOMPARE(sink.outputStatus().acceptedFrames, qint64(1));
    QCOMPARE(sink.outputStatus().lastDeliveredFrameIndex, qint64(11));
}

QTEST_GUILESS_MAIN(TestAjaSink)
#include "tst_ajasink.moc"
