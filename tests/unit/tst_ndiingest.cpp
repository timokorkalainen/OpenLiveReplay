#include <QtTest>

#include "recorder_engine/ingest/nativendiingestsession.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
}

class FakeNdiReceiverBackend final : public INdiReceiverBackend {
public:
    struct Item {
        Capture type = Capture::None;
        NdiVideoFrame video;
        NdiAudioFrame audio;
    };

    bool isRuntimeAvailable() const override { return available; }
    bool openReceiver(const QString& sourceName) override {
        openedName = sourceName;
        opened = available && !sourceName.isEmpty();
        return opened;
    }
    void closeReceiver() override {
        // Detect the original UAF: a close that overlaps an in-flight capture().
        if (inCapture.load(std::memory_order_relaxed)) {
            closedDuringCapture.store(true, std::memory_order_relaxed);
        }
        opened = false;
        closeThread = std::this_thread::get_id();
        closeCount.fetch_add(1, std::memory_order_relaxed);
    }
    Capture capture(NdiVideoFrame* video, NdiAudioFrame* audio, int) override {
        captureThread = std::this_thread::get_id();
        if (blockingCapture) {
            // Hold an "in capture" window so a concurrent close (the old UAF) is
            // observable and TSan can witness the race on the receiver handle.
            inCapture.store(true, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            inCapture.store(false, std::memory_order_relaxed);
        }
        if (items.isEmpty()) {
            return Capture::None;
        }
        const Item item = items.takeFirst();
        if (item.type == Capture::Video && video) *video = item.video;
        if (item.type == Capture::Audio && audio) *audio = item.audio;
        return item.type;
    }
    void freeVideo(NdiVideoFrame*) override { ++freedVideo; }
    void freeAudio(NdiAudioFrame*) override { ++freedAudio; }

    bool available = true;
    bool opened = false;
    QString openedName;
    QList<Item> items;
    int freedVideo = 0;
    int freedAudio = 0;
    bool blockingCapture = false; // set-before-run: make capture() hold a window
    std::atomic<int> closeCount{0};
    std::atomic<bool> inCapture{false};
    std::atomic<bool> closedDuringCapture{false};
    std::thread::id closeThread;   // written by closeReceiver(); read after join()
    std::thread::id captureThread; // written by capture(); read after join()
};

class TestNdiIngest : public QObject {
    Q_OBJECT

private slots:
    void supportsNdiUrls();
    void unavailableRuntimeFailsOpen();
    void fakeBackendDeliversVideoAndAudio();
    void localRecvCreateStructMatchesSdkShape();
    void macRuntimeCandidatesIncludeOfficialSdkPath();
    void exactSourceMatchWinsBeforeSubstringFallback();
    void undefinedTimestampFallsBackToArrivalWithoutLockingClock();
    void stallTimerBreaksOnSilence();
    void requestStopIsFlagOnlyAndClosesOnRunThread();
};

void TestNdiIngest::supportsNdiUrls() {
    QVERIFY(NativeNdiIngestSession::supportsUrl(QUrl(QStringLiteral("ndi:Studio%20%28CAM1%29"))));
    QVERIFY(NativeNdiIngestSession::supportsUrl(QUrl(QStringLiteral("ndi://cam1"))));
    QCOMPARE(
        NativeNdiIngestSession::sourceNameFromUrl(QUrl(QStringLiteral("ndi:Studio%20%28CAM1%29"))),
        QStringLiteral("Studio (CAM1)"));
    QVERIFY(!NativeNdiIngestSession::supportsUrl(QUrl(QStringLiteral("rtmp://x/live/a"))));
}

void TestNdiIngest::unavailableRuntimeFailsOpen() {
    FakeNdiReceiverBackend backend;
    backend.available = false;
    std::atomic<bool> running{true};
    NativeNdiIngestSession session(0, 4, 4, &running, &backend);
    IngestCallbacks callbacks;
    QVERIFY(!session.open(QUrl(QStringLiteral("ndi://CAM1")), callbacks));
    QCOMPARE(session.lastFailureKind(), IngestFailureKind::DecodeCapability);
}

void TestNdiIngest::fakeBackendDeliversVideoAndAudio() {
    QByteArray pixels;
    pixels.append(QByteArray(16, char(50)));
    pixels.append(QByteArray(4, char(60)));
    pixels.append(QByteArray(4, char(70)));
    QVector<float> audio(8);
    for (int i = 0; i < 4; ++i) {
        audio[i] = 0.5f;
        audio[4 + i] = -0.5f;
    }

    FakeNdiReceiverBackend backend;
    FakeNdiReceiverBackend::Item videoItem;
    videoItem.type = INdiReceiverBackend::Capture::Video;
    videoItem.video.width = 4;
    videoItem.video.height = 4;
    videoItem.video.strideBytes = 4;
    videoItem.video.fourCc = kNdiFourCcI420;
    videoItem.video.data = reinterpret_cast<const uint8_t*>(pixels.constData());
    videoItem.video.timestamp100ns = 1000 * 10000LL;
    videoItem.video.timecode100ns = 1234567;
    backend.items.append(videoItem);

    FakeNdiReceiverBackend::Item audioItem;
    audioItem.type = INdiReceiverBackend::Capture::Audio;
    audioItem.audio.sampleRate = 48000;
    audioItem.audio.channels = 2;
    audioItem.audio.samples = 4;
    audioItem.audio.channelStrideBytes = 4 * int(sizeof(float));
    audioItem.audio.data = audio.constData();
    audioItem.audio.timestamp100ns = 1000 * 10000LL;
    audioItem.audio.timecode100ns = 2345678;
    backend.items.append(audioItem);

    std::atomic<bool> running{true};
    NativeNdiIngestSession session(0, 4, 4, &running, &backend);
    QList<int64_t> videoPts;
    QList<int64_t> videoTimecodes;
    QList<int64_t> audioStarts;
    QList<int64_t> audioTimecodes;
    QList<IngestStats> statsReports;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = []() { return int64_t(5000); };
    callbacks.reportStats = [&statsReports](const IngestStats& stats) {
        statsReports.append(stats);
    };
    callbacks.onVideoFrame = [&videoPts, &videoTimecodes](DecodedVideoFrame decoded) {
        videoPts.append(decoded.sourcePtsMs);
        videoTimecodes.append(decoded.sourceTimecode100ns);
        QCOMPARE(decoded.frame->width, 4);
        QCOMPARE(uchar(decoded.frame->data[0][0]), uchar(50));
        av_frame_free(&decoded.frame);
    };
    callbacks.onAudioChunk = [&audioStarts, &audioTimecodes, &running](DecodedAudioChunk chunk) {
        audioStarts.append(chunk.startSample);
        audioTimecodes.append(chunk.sourceTimecode100ns);
        QCOMPARE(chunk.pcmS16Stereo.size(), 4 * 2 * int(sizeof(int16_t)));
        running.store(false, std::memory_order_relaxed);
    };

    QVERIFY(session.open(QUrl(QStringLiteral("ndi:CAM1")), callbacks));
    QCOMPARE(backend.openedName, QStringLiteral("CAM1"));
    session.run();

    QCOMPARE(videoPts.size(), 1);
    QCOMPARE(audioStarts.size(), 1);
    QCOMPARE(videoPts.first(), int64_t(5000));
    QCOMPARE(videoTimecodes.first(), int64_t(1234567));
    QCOMPARE(audioStarts.first(), int64_t(5000 * 48000 / 1000));
    QCOMPARE(audioTimecodes.first(), int64_t(2345678));
    QVERIFY(!statsReports.isEmpty());
    QCOMPARE(statsReports.last().kind, IngestStatsKind::Ndi);
    QCOMPARE(statsReports.last().clockQuality, int(ClockQuality::Ndi));
    // The NDI clock anchors on the first authority observation (sender 1000 ms vs
    // session 5000 ms), so it reports locked with a session-minus-sender offset.
    QVERIFY(statsReports.last().clockLocked);
    QCOMPARE(statsReports.last().clockOffsetNs, int64_t(4000) * 1000000LL);
    QCOMPARE(backend.freedVideo, 1);
    QCOMPARE(backend.freedAudio, 1);
}

void TestNdiIngest::localRecvCreateStructMatchesSdkShape() {
    QVERIFY(NativeNdiIngestSession::recvCreateSourceIsValueForTest());
}

void TestNdiIngest::macRuntimeCandidatesIncludeOfficialSdkPath() {
    const QStringList candidates = NativeNdiIngestSession::runtimeLibraryCandidatesForTest();
#if defined(Q_OS_MACOS)
    QVERIFY(
        candidates.contains(QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib")));
#else
    QVERIFY(!candidates.isEmpty());
#endif
}

void TestNdiIngest::exactSourceMatchWinsBeforeSubstringFallback() {
    const QString selected = NativeNdiIngestSession::selectDiscoveredSourceForTest(
        QStringList{QStringLiteral("Studio CAM10"), QStringLiteral("CAM1")},
        QStringLiteral("CAM1"));
    QCOMPARE(selected, QStringLiteral("CAM1"));

    const QString fallback = NativeNdiIngestSession::selectDiscoveredSourceForTest(
        QStringList{QStringLiteral("Studio CAM10")}, QStringLiteral("CAM1"));
    QCOMPARE(fallback, QStringLiteral("Studio CAM10"));
}

void TestNdiIngest::undefinedTimestampFallsBackToArrivalWithoutLockingClock() {
    QByteArray pixels;
    pixels.append(QByteArray(16, char(50)));
    pixels.append(QByteArray(4, char(60)));
    pixels.append(QByteArray(4, char(70)));

    FakeNdiReceiverBackend backend;
    FakeNdiReceiverBackend::Item videoItem;
    videoItem.type = INdiReceiverBackend::Capture::Video;
    videoItem.video.width = 4;
    videoItem.video.height = 4;
    videoItem.video.strideBytes = 4;
    videoItem.video.fourCc = kNdiFourCcI420;
    videoItem.video.data = reinterpret_cast<const uint8_t*>(pixels.constData());
    videoItem.video.timestamp100ns = std::numeric_limits<int64_t>::max();
    backend.items.append(videoItem);

    std::atomic<bool> running{true};
    NativeNdiIngestSession session(0, 4, 4, &running, &backend);
    QList<int64_t> videoPts;
    QList<IngestStats> statsReports;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = []() { return int64_t(7777); };
    callbacks.reportStats = [&statsReports](const IngestStats& stats) {
        statsReports.append(stats);
    };
    callbacks.onVideoFrame = [&videoPts, &running](DecodedVideoFrame decoded) {
        videoPts.append(decoded.sourcePtsMs);
        av_frame_free(&decoded.frame);
        running.store(false, std::memory_order_relaxed);
    };

    QVERIFY(session.open(QUrl(QStringLiteral("ndi:CAM1")), callbacks));
    session.run();

    QCOMPARE(videoPts.size(), 1);
    QCOMPARE(videoPts.first(), int64_t(7777));
    QVERIFY(!statsReports.isEmpty());
    QCOMPARE(statsReports.last().clockPpm, 0.0);
    // An undefined timestamp never anchors the clock: it stays unlocked with a
    // zero offset (the additive fields default safely).
    QVERIFY(!statsReports.last().clockLocked);
    QCOMPARE(statsReports.last().clockOffsetNs, int64_t(0));
}

void TestNdiIngest::stallTimerBreaksOnSilence() {
    // A source that connects but never delivers a frame (capture()==None forever)
    // must trip the session-internal stall timer: run() returns and classifies the
    // failure as TransientNetwork so captureLoop reconnects. A short test-only
    // timeout keeps this from waiting the production 8s window.
    FakeNdiReceiverBackend backend; // no items -> Capture::None forever
    std::atomic<bool> running{true};
    NativeNdiIngestSession session(0, 4, 4, &running, &backend);
    session.setStallTimeoutMsForTest(20);

    QStringList logs;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = []() { return int64_t(0); };
    callbacks.logInfo = [&logs](const QString& msg) { logs.append(msg); };

    QVERIFY(session.open(QUrl(QStringLiteral("ndi:CAM1")), callbacks));
    QElapsedTimer t;
    t.start();
    session.run(); // must return on its own (stall), not hang
    QVERIFY(t.elapsed() < 5000);
    QCOMPARE(session.lastFailureKind(), IngestFailureKind::TransientNetwork);
    QVERIFY(logs.contains(QStringLiteral("Native NDI stalled. Restarting...")));
}

void TestNdiIngest::requestStopIsFlagOnlyAndClosesOnRunThread() {
    // The shutdown UAF fix: requestStop() must NOT destroy the receiver (it runs on
    // the stopping thread while the capture thread is inside capture()). The receiver
    // is destroyed only by run() on the capture thread, after it observes the flag.
    FakeNdiReceiverBackend backend; // Capture::None forever -> run() loops until stop
    backend.blockingCapture = true; // each capture() holds a ~5ms window
    std::atomic<bool> running{true};
    NativeNdiIngestSession session(0, 4, 4, &running, &backend);
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = []() { return int64_t(0); };
    QVERIFY(session.open(QUrl(QStringLiteral("ndi:CAM1")), callbacks));

    std::thread::id runThread;
    std::thread runner([&]() {
        runThread = std::this_thread::get_id();
        session.run();
    });

    // Let run() loop on the capture thread for a while (default 8s stall window
    // keeps it running). It must not have closed the receiver while looping.
    QTest::qWait(40);
    QCOMPARE(backend.closeCount.load(std::memory_order_relaxed), 0);

    // Hammer requestStop() from THIS (main) thread repeatedly across capture
    // windows. On the OLD code each call destroyed the receiver from the main
    // thread, mid-capture() — setting closedDuringCapture, closing from the wrong
    // thread, and racing the handle (TSan-visible). On the fixed code requestStop()
    // only flips a flag; run() closes exactly once, on its own thread, after the
    // loop. (We do NOT assert closeCount here: run() closes asynchronously the
    // moment it observes the flag, which can race this loop — that's correct.)
    for (int i = 0; i < 20; ++i) {
        session.requestStop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runner.join(); // run() has observed the flag, exited, and closed on its thread

    QCOMPARE(backend.closeCount.load(std::memory_order_relaxed), 1); // exactly once
    QVERIFY(backend.closeThread == runThread);                       // closed on the capture thread
    QVERIFY(backend.closeThread != std::this_thread::get_id());      // NOT the stopping thread
    QVERIFY(!backend.closedDuringCapture.load(std::memory_order_relaxed)); // never raced capture()
}

QTEST_GUILESS_MAIN(TestNdiIngest)
#include "tst_ndiingest.moc"
