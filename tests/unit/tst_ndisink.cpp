#include <QtTest>

#include "playback/output/ndisink.h"

static MediaVideoFrame video(int feed, qint64 pts, uchar y) {
    MediaVideoFrame f = MediaVideoFrame::solidYuv420p(4, 4, y, 128, 128);
    f.feedIndex = feed;
    f.ptsMs = pts;
    f.outputFrameIndex = 7;
    return f;
}

class FakeNdiBackend final : public INdiSenderBackend {
public:
    explicit FakeNdiBackend(bool available) : runtimeAvailable(available) {}

    bool isRuntimeAvailable() const override { return runtimeAvailable; }

    bool createSender(const QString& senderName, FrameRate rate) override {
        createdName = senderName;
        createdRate = rate;
        active = runtimeAvailable && createSucceeds && !senderName.isEmpty() && rate.isValid();
        return active;
    }

    void destroySender() override { active = false; }

    bool sendFrame(const OutputBusFrame& frame) override {
        if (!active || !sendSucceeds) return false;
        sentFrames.append(frame);
        return true;
    }

    bool runtimeAvailable = false;
    bool createSucceeds = true;
    bool sendSucceeds = true;
    bool active = false;
    QString createdName;
    FrameRate createdRate;
    QVector<OutputBusFrame> sentFrames;
};

class TestNdiSink : public QObject {
    Q_OBJECT
private slots:
    void unavailableRuntimeFailsCleanly();
    void startUsesConfiguredSenderNameAndSubmitsCleanBusFrames();
    void rejectsDisabledOrNonNdiAssignments();
    void reportsCreateFailureAndStoppedStatus();
    void reportsSendFailureStatus();
};

void TestNdiSink::unavailableRuntimeFailsCleanly() {
    FakeNdiBackend backend(false);
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(!sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(!sink.isActive());
    QCOMPARE(sink.status().state, NdiOutputState::RuntimeUnavailable);
    QVERIFY(sink.status().message.contains(QStringLiteral("runtime"), Qt::CaseInsensitive));
    QVERIFY(!sink.submit(OutputBusFrame{}));
    QCOMPARE(backend.sentFrames.size(), 0);
}

void TestNdiSink::startUsesConfiguredSenderNameAndSubmitsCleanBusFrames() {
    FakeNdiBackend backend(true);
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(30000, 1001)));
    QVERIFY(sink.isActive());
    QCOMPARE(sink.status().state, NdiOutputState::Active);
    QCOMPARE(backend.createdName, QStringLiteral("OLR Feed 1"));
    QCOMPARE(backend.createdRate.numerator, 30000);
    QCOMPARE(backend.createdRate.denominator, 1001);

    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = 7;
    frame.sampledPlayheadMs = 280;
    frame.video = video(0, 280, 90);
    frame.audio.feedIndex = 0;
    frame.audio.sampleRate = 48000;
    frame.audio.channels = 2;
    frame.audio.format = MediaSampleFormat::S16Interleaved;
    frame.audio.pcm = QByteArray(8 * int(sizeof(qint16)), '\0');

    QVERIFY(sink.submit(frame));
    QCOMPARE(sink.status().state, NdiOutputState::Active);
    QCOMPARE(sink.status().framesSubmitted, qint64(1));
    QCOMPARE(backend.sentFrames.size(), 1);
    QCOMPARE(backend.sentFrames[0].bus, OutputBusId::feed(0));
    QCOMPARE(backend.sentFrames[0].outputFrameIndex, qint64(7));
    QCOMPARE(uchar(backend.sentFrames[0].video.planeY.at(0)), uchar(90));
}

void TestNdiSink::rejectsDisabledOrNonNdiAssignments() {
    FakeNdiBackend backend(true);
    NdiOutputSink sink(&backend);

    OutputTargetAssignment disabled;
    disabled.kind = OutputTargetKind::Ndi;
    disabled.enabled = false;
    disabled.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Disabled"));
    QVERIFY(!sink.start(disabled, FrameRate::fromFraction(25, 1)));

    OutputTargetAssignment wrongKind;
    wrongKind.kind = OutputTargetKind::DeckLinkSdiHdmi;
    wrongKind.enabled = true;
    wrongKind.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Wrong"));
    QVERIFY(!sink.start(wrongKind, FrameRate::fromFraction(25, 1)));
    QCOMPARE(sink.status().state, NdiOutputState::InvalidAssignment);
    QCOMPARE(backend.sentFrames.size(), 0);
}

void TestNdiSink::reportsCreateFailureAndStoppedStatus() {
    FakeNdiBackend backend(true);
    backend.createSucceeds = false;
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(!sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QCOMPARE(sink.status().state, NdiOutputState::CreateFailed);
    QVERIFY(sink.status().message.contains(QStringLiteral("create"), Qt::CaseInsensitive));

    backend.createSucceeds = true;
    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    sink.stop();
    QCOMPARE(sink.status().state, NdiOutputState::Stopped);
    QVERIFY(!sink.isActive());
}

void TestNdiSink::reportsSendFailureStatus() {
    FakeNdiBackend backend(true);
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    backend.sendSucceeds = false;

    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = 9;
    frame.sampledPlayheadMs = 360;
    frame.video = video(0, 360, 80);
    frame.audio.feedIndex = 0;
    frame.audio.sampleRate = 48000;
    frame.audio.channels = 2;
    frame.audio.format = MediaSampleFormat::S16Interleaved;
    frame.audio.pcm = QByteArray(8 * int(sizeof(qint16)), '\0');

    QVERIFY(!sink.submit(frame));
    QCOMPARE(sink.status().state, NdiOutputState::SendFailed);
    QCOMPARE(sink.status().sendFailures, qint64(1));
    QCOMPARE(sink.status().lastFrameIdentity.outputFrameIndex, qint64(9));
}

QTEST_GUILESS_MAIN(TestNdiSink)
#include "tst_ndisink.moc"
