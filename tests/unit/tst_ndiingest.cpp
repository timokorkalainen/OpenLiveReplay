#include <QtTest>

#include "recorder_engine/ingest/nativendiingestsession.h"

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
    void closeReceiver() override { opened = false; }
    Capture capture(NdiVideoFrame* video, NdiAudioFrame* audio, int) override {
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
};

class TestNdiIngest : public QObject {
    Q_OBJECT

private slots:
    void supportsNdiUrls();
    void unavailableRuntimeFailsOpen();
    void fakeBackendDeliversVideoAndAudio();
};

void TestNdiIngest::supportsNdiUrls() {
    QVERIFY(NativeNdiIngestSession::supportsUrl(QUrl(QStringLiteral("ndi:Studio%20%28CAM1%29"))));
    QVERIFY(NativeNdiIngestSession::supportsUrl(QUrl(QStringLiteral("ndi://cam1"))));
    QCOMPARE(NativeNdiIngestSession::sourceNameFromUrl(
                 QUrl(QStringLiteral("ndi:Studio%20%28CAM1%29"))),
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
    backend.items.append(videoItem);

    FakeNdiReceiverBackend::Item audioItem;
    audioItem.type = INdiReceiverBackend::Capture::Audio;
    audioItem.audio.sampleRate = 48000;
    audioItem.audio.channels = 2;
    audioItem.audio.samples = 4;
    audioItem.audio.channelStrideBytes = 4 * int(sizeof(float));
    audioItem.audio.data = audio.constData();
    audioItem.audio.timestamp100ns = 1000 * 10000LL;
    backend.items.append(audioItem);

    std::atomic<bool> running{true};
    NativeNdiIngestSession session(0, 4, 4, &running, &backend);
    QList<int64_t> videoPts;
    QList<int64_t> audioStarts;
    IngestCallbacks callbacks;
    callbacks.recordingClockMs = []() { return int64_t(5000); };
    callbacks.onVideoFrame = [&videoPts](DecodedVideoFrame decoded) {
        videoPts.append(decoded.sourcePtsMs);
        QCOMPARE(decoded.frame->width, 4);
        QCOMPARE(uchar(decoded.frame->data[0][0]), uchar(50));
        av_frame_free(&decoded.frame);
    };
    callbacks.onAudioChunk = [&audioStarts, &running](DecodedAudioChunk chunk) {
        audioStarts.append(chunk.startSample);
        QCOMPARE(chunk.pcmS16Stereo.size(), 4 * 2 * int(sizeof(int16_t)));
        running.store(false, std::memory_order_relaxed);
    };

    QVERIFY(session.open(QUrl(QStringLiteral("ndi:CAM1")), callbacks));
    QCOMPARE(backend.openedName, QStringLiteral("CAM1"));
    session.run();

    QCOMPARE(videoPts.size(), 1);
    QCOMPARE(audioStarts.size(), 1);
    QCOMPARE(videoPts.first(), int64_t(5000));
    QCOMPARE(audioStarts.first(), int64_t(5000 * 48000 / 1000));
    QCOMPARE(backend.freedVideo, 1);
    QCOMPARE(backend.freedAudio, 1);
}

QTEST_GUILESS_MAIN(TestNdiIngest)
#include "tst_ndiingest.moc"
