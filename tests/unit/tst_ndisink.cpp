#include <QtTest>

#include "playback/output/ndiabi.h"
#include "playback/output/ndisink.h"
#include "playback/output/ndiruntimepaths.h"

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

namespace {

OutputTargetAssignment ndiAssignment() {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));
    return assignment;
}

OutputBusFrame validFrame(qint64 outputFrameIndex, qint64 playheadMs, uchar y) {
    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = outputFrameIndex;
    frame.sampledPlayheadMs = playheadMs;
    frame.video = solidYuv420pHandle(4, 4, y, 128, 128);
    frame.video.metadata().key.feedIndex = 0;
    frame.video.metadata().key.ptsMs = playheadMs;
    frame.video.metadata().outputFrameIndex = outputFrameIndex;
    frame.audio.feedIndex = 0;
    frame.audio.sampleRate = 48000;
    frame.audio.channels = 2;
    frame.audio.format = MediaSampleFormat::S16Interleaved;
    frame.audio.pcm = QByteArray(8 * int(sizeof(qint16)), '\0');
    return frame;
}

} // namespace

static FrameHandle video(int feed, qint64 pts, uchar y) {
    FrameHandle f = solidYuv420pHandle(4, 4, y, 128, 128);
    f.metadata().key.feedIndex = feed;
    f.metadata().key.ptsMs = pts;
    f.metadata().outputFrameIndex = 7;
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
        if (sendDelayMs > 0) QThread::msleep(uint(sendDelayMs));
        if (blockSend) {
            QMutexLocker locker(&sendMutex);
            sendEntered = true;
            sendEnteredCondition.wakeAll();
            while (!releaseSend) {
                releaseSendCondition.wait(&sendMutex);
            }
        }
        if (!active || !sendSucceeds) return false;
        sentFrames.append(frame);
        return true;
    }

    bool waitForSendEntered(int timeoutMs) {
        QMutexLocker locker(&sendMutex);
        if (sendEntered) return true;
        return sendEnteredCondition.wait(&sendMutex, timeoutMs);
    }

    void releaseBlockedSend() {
        QMutexLocker locker(&sendMutex);
        releaseSend = true;
        releaseSendCondition.wakeAll();
    }

    bool runtimeAvailable = false;
    bool createSucceeds = true;
    bool sendSucceeds = true;
    bool active = false;
    int sendDelayMs = 0;
    bool blockSend = false;
    QString createdName;
    FrameRate createdRate;
    QVector<OutputBusFrame> sentFrames;

private:
    QMutex sendMutex;
    QWaitCondition sendEnteredCondition;
    QWaitCondition releaseSendCondition;
    bool sendEntered = false;
    bool releaseSend = false;
};

class TestNdiSink : public QObject {
    Q_OBJECT
private slots:
    void runtimeCandidatesIncludeNdiToolsInstallLocations();
    void unavailableRuntimeFailsCleanly();
    void startUsesConfiguredSenderNameAndSubmitsCleanBusFrames();
    void rejectsFramesWithoutBroadcastAudio();
    void rejectsDisabledOrNonNdiAssignments();
    void reportsCreateFailureAndStoppedStatus();
    void reportsSendFailureStatus();
    void reportsSendDurationInOutputStatus();
    void failedSendReportsAttemptedButNotDeliveredFrame();
    void inFlightSendReportsAttemptedButNotDeliveredFrame();
    void ndiFrameTimingStampsSharedProgrammeTimecode();
};

void TestNdiSink::runtimeCandidatesIncludeNdiToolsInstallLocations() {
    const QStringList candidates = olr::ndi::runtimeLibraryCandidates();
#if defined(Q_OS_MACOS)
    QVERIFY(candidates.contains(
        QStringLiteral("/Applications/NDI Scan Converter.app/Contents/Frameworks/libndi.dylib")));
    QVERIFY(
        candidates.contains(QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib")));
#else
    QVERIFY(!candidates.isEmpty());
#endif
}

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
    // Distinct from sampledPlayheadMs*10000 (=2,800,000) so a regression that fed the wrong
    // source field into the sink would be caught.
    frame.programmeTimecode100ns = 2000000;
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
    QCOMPARE(uchar(MediaVideoFrameView(backend.sentFrames[0].video).planeY.at(0)), uchar(90));
    // The programme timecode survives the submit -> backend path unscaled/unswapped.
    QCOMPARE(backend.sentFrames[0].programmeTimecode100ns, qint64(2000000));
}

void TestNdiSink::rejectsFramesWithoutBroadcastAudio() {
    FakeNdiBackend backend(true);
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));

    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = 8;
    frame.sampledPlayheadMs = 320;
    frame.video = video(0, 320, 100);

    QVERIFY(!sink.submit(frame));
    QCOMPARE(sink.status().state, NdiOutputState::SendFailed);
    QCOMPARE(sink.status().framesSubmitted, qint64(0));
    QCOMPARE(sink.status().sendFailures, qint64(1));
    QCOMPARE(backend.sentFrames.size(), 0);
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

void TestNdiSink::reportsSendDurationInOutputStatus() {
    FakeNdiBackend backend(true);
    backend.sendDelayMs = 5;
    NdiOutputSink sink(&backend);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-ndi");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), QStringLiteral("OLR Feed 1"));

    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));

    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = 42;
    frame.sampledPlayheadMs = 1680;
    frame.video = video(0, 1680, 96);
    frame.audio.feedIndex = 0;
    frame.audio.sampleRate = 48000;
    frame.audio.channels = 2;
    frame.audio.format = MediaSampleFormat::S16Interleaved;
    frame.audio.pcm = QByteArray(8 * int(sizeof(qint16)), '\0');

    QVERIFY(sink.submit(frame));
    const OutputSinkStatus status = sink.outputStatus();
    QVERIFY(status.lastSubmitDurationNs > 0);
    QVERIFY(status.hasLastQueuedFrameIndex);
    QCOMPARE(status.lastQueuedFrameIndex, qint64(42));
    QVERIFY(status.hasLastDeliveredFrameIndex);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(42));
}

void TestNdiSink::failedSendReportsAttemptedButNotDeliveredFrame() {
    FakeNdiBackend backend(true);
    NdiOutputSink sink(&backend);

    QVERIFY(sink.start(ndiAssignment(), FrameRate::fromFraction(25, 1)));
    backend.sendSucceeds = false;

    QVERIFY(!sink.submit(validFrame(43, 1720, 104)));

    const OutputSinkStatus status = sink.outputStatus();
    QVERIFY(status.hasLastQueuedFrameIndex);
    QCOMPARE(status.lastQueuedFrameIndex, qint64(43));
    QVERIFY(!status.hasLastDeliveredFrameIndex);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(-1));
}

void TestNdiSink::inFlightSendReportsAttemptedButNotDeliveredFrame() {
    FakeNdiBackend backend(true);
    backend.blockSend = true;
    NdiOutputSink sink(&backend);

    QVERIFY(sink.start(ndiAssignment(), FrameRate::fromFraction(25, 1)));

    bool submitResult = false;
    QThread* submitThread =
        QThread::create([&]() { submitResult = sink.submit(validFrame(44, 1760, 112)); });
    submitThread->start();

    const bool sendEntered = backend.waitForSendEntered(1000);
    const OutputSinkStatus status = sink.outputStatus();

    backend.releaseBlockedSend();
    const bool submitFinished = submitThread->wait(1000);
    delete submitThread;

    QVERIFY(sendEntered);
    QVERIFY(status.hasLastQueuedFrameIndex);
    QCOMPARE(status.lastQueuedFrameIndex, qint64(44));
    QVERIFY(!status.hasLastDeliveredFrameIndex);
    QCOMPARE(status.lastDeliveredFrameIndex, qint64(-1));
    QVERIFY(submitFinished);
    QVERIFY(submitResult);
}

// The NDI sink maps the bus frame's programme timecode onto BOTH the video and audio frames
// (shared, so the A/V pair stays aligned) and falls back to the SDK "synthesize" sentinel when
// the bus left it unset (-1). This drives applyNdiFrameTiming with a real OutputBusFrame — the
// exact bus-frame -> NDI-struct mapping the sink performs per frame — without a live NDI
// runtime. The NDI `timestamp` is the SDK's to fill on send, so it is not asserted here.
void TestNdiSink::ndiFrameTimingStampsSharedProgrammeTimecode() {
    // resolveNdiTimecode: passthrough for a real programme TC, synthesize for the unset -1.
    QCOMPARE(resolveNdiTimecode(0), qint64(0));
    QCOMPARE(resolveNdiTimecode(2000000), qint64(2000000)); // 200 ms playhead → 100 ns units
    QCOMPARE(resolveNdiTimecode(-1), olr::ndi::kTimecodeSynthesize);

    // A real programme TC on the bus frame reaches both NDI structs, shared.
    OutputBusFrame frame = validFrame(7, /*playheadMs=*/200, 60);
    frame.programmeTimecode100ns = 2000000;
    olr::ndi::NDIlib_video_frame_v2_t video;
    olr::ndi::NDIlib_audio_frame_v3_t audio;
    applyNdiFrameTiming(frame, video, audio);
    QCOMPARE(video.timecode, qint64(2000000)); // the frame's programme TC reaches the struct
    QCOMPARE(audio.timecode, video.timecode);  // A/V share the tick's timecode

    // Unset programme TC (-1) → synthesize on both. Poison the fields first so the WRITE is
    // proven (the struct default is already kTimecodeSynthesize, which would pass vacuously).
    OutputBusFrame unset = validFrame(8, /*playheadMs=*/0, 60);
    unset.programmeTimecode100ns = -1;
    olr::ndi::NDIlib_video_frame_v2_t video2;
    olr::ndi::NDIlib_audio_frame_v3_t audio2;
    video2.timecode = audio2.timecode = qint64(42);
    applyNdiFrameTiming(unset, video2, audio2);
    QCOMPARE(video2.timecode, olr::ndi::kTimecodeSynthesize);
    QCOMPARE(audio2.timecode, olr::ndi::kTimecodeSynthesize);
}

QTEST_GUILESS_MAIN(TestNdiSink)
#include "tst_ndisink.moc"
