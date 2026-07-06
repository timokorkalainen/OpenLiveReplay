// OmtOutputSink is a software-SDK CPU-frame continuous-cadence sibling of NDI.
// Off-SDK it reports RuntimeUnavailable; an injected fake records sender
// creation and receives one bus frame per submit.
#include <QtTest>

#include "playback/output/framehandle.h"
#include "playback/output/iotargets/omtsink.h"
#include "playback/output/outputbusengine.h"

namespace {

class FakeOmtBackend final : public IOmtSenderBackend {
public:
    bool available = true;
    QString lastSenderName;
    FrameRate lastRate;
    int sent = 0;

    bool isRuntimeAvailable() const override { return available; }
    bool createSender(const QString& senderName, FrameRate rate) override {
        lastSenderName = senderName;
        lastRate = rate;
        return available;
    }
    void destroySender() override {}
    bool sendFrame(const OutputBusFrame&) override {
        ++sent;
        return true;
    }
};

OutputTargetAssignment omtAssignment() {
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::Omt;
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR OMT 1"));
    return assignment;
}

} // namespace

class TestOmtSink : public QObject {
    Q_OBJECT
private slots:
    void stubReportsRuntimeUnavailable();
    void capabilityIsContinuousCadence();
    void requestsContinuousCadence();
    void submitSendsFrameWithConfiguredSenderName();
};

void TestOmtSink::stubReportsRuntimeUnavailable() {
    OmtOutputSink sink;

    QVERIFY(!sink.start(omtAssignment(), FrameRate::fromFraction(50, 1)));
    QCOMPARE(sink.outputStatus().state, QStringLiteral("runtime-unavailable"));
}

void TestOmtSink::capabilityIsContinuousCadence() {
    OmtOutputSink sink;

    QCOMPARE(sink.resolvedCapability(), SinkGpuCapability::NeedsContinuousCadence);
}

void TestOmtSink::requestsContinuousCadence() {
    OmtOutputSink sink;

    QVERIFY(sink.needsContinuousCadence());
}

void TestOmtSink::submitSendsFrameWithConfiguredSenderName() {
    FakeOmtBackend backend;
    OmtOutputSink sink(&backend);
    OutputBusFrame frame;
    frame.outputFrameIndex = 13;
    frame.video = solidYuv420pHandle(64, 48, 16, 128, 128);

    QVERIFY(sink.start(omtAssignment(), FrameRate::fromFraction(50, 1)));
    QCOMPARE(backend.lastSenderName, QStringLiteral("OLR OMT 1"));
    QCOMPARE(backend.lastRate.numerator, 50);
    QVERIFY(sink.submit(frame));
    QCOMPARE(backend.sent, 1);
    QCOMPARE(sink.outputStatus().acceptedFrames, qint64(1));
    QCOMPARE(sink.outputStatus().lastDeliveredFrameIndex, qint64(13));
}

QTEST_GUILESS_MAIN(TestOmtSink)
#include "tst_omtsink.moc"
