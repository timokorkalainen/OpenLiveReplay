#include "playback/output/ndisink.h"
#include "playback/output/ndiabi.h"
#include "playback/output/ndiruntimepaths.h"
#include "playback/output/outputdispatcher.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLibrary>
#include <QThread>
#include <QtTest>

#include <cmath>

namespace {

using namespace olr::ndi;

struct CaptureStats {
    int videoFrames = 0;
    int audioFrames = 0;
    bool sawNonSilentAudio = false;
};

MediaAudioFrame makeAudio(qint64 startSample, int sampleFrames) {
    MediaAudioFrame audio;
    audio.feedIndex = 0;
    audio.startSample = startSample;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm.resize(sampleFrames * audio.channels * int(sizeof(qint16)));
    auto* out = reinterpret_cast<qint16*>(audio.pcm.data());
    for (int sample = 0; sample < sampleFrames; ++sample) {
        const auto value = qint16(std::lround(std::sin(double(sample) * 0.05) * 12000.0));
        out[sample * 2] = value;
        out[sample * 2 + 1] = qint16(-value);
    }
    return audio;
}

void insertSourceFrame(OutputFrameCache* cache, qint64 index) {
    constexpr qint64 kFrameDurationMs = 40;
    constexpr int kAudioSamplesPerFrame = 1920;

    MediaVideoFrame video =
        MediaVideoFrame::solidYuv420p(16, 16, uchar(70 + (index % 120)), 96, 160);
    video.feedIndex = 0;
    video.ptsMs = index * kFrameDurationMs;
    cache->insertVideoFrame(video);
    cache->insertAudioFrame(makeAudio(index * kAudioSamplesPerFrame, kAudioSamplesPerFrame));
}

class NdiRuntimeReceiver final {
public:
    ~NdiRuntimeReceiver() { close(); }

    bool load() {
        for (const QString& candidate : runtimeLibraryCandidates()) {
            if (candidate.isEmpty()) continue;
            m_library.setFileName(candidate);
            if (!m_library.load()) continue;
            if (resolveSymbols() && (!m_initialize || m_initialize())) {
                m_loadedPath = candidate;
                m_initialized = true;
                return true;
            }
            m_library.unload();
        }
        return false;
    }

    QString loadedPath() const { return m_loadedPath; }

    bool hasRequiredSymbols() const {
        return m_findCreate && m_findDestroy && m_findWaitForSources && m_findGetCurrentSources &&
               m_recvCreate && m_recvDestroy && m_recvCapture && m_recvFreeVideo && m_recvFreeAudio;
    }

    bool createFinder() {
        closeFinder();
        NDIlib_find_create_t create;
        create.show_local_sources = true;
        m_find = m_findCreate(&create);
        return m_find != nullptr;
    }

    bool waitForSource(const QString& senderName, int timeoutMs) {
        m_sourceName.clear();
        m_sourceUrl.clear();

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            m_findWaitForSources(m_find, 250);

            quint32 sourceCount = 0;
            const NDIlib_source_t* sources = m_findGetCurrentSources(m_find, &sourceCount);
            for (quint32 i = 0; i < sourceCount; ++i) {
                const QString ndiName = QString::fromUtf8(sources[i].p_ndi_name);
                if (ndiName == senderName ||
                    ndiName.contains(QStringLiteral("(%1)").arg(senderName)) ||
                    ndiName.contains(senderName)) {
                    m_sourceName = QByteArray(sources[i].p_ndi_name);
                    if (sources[i].p_url_address)
                        m_sourceUrl = QByteArray(sources[i].p_url_address);
                    return true;
                }
            }
        }
        return false;
    }

    bool createReceiver() {
        closeReceiver();
        if (m_sourceName.isEmpty()) return false;

        NDIlib_recv_create_v3_t create;
        create.source_to_connect_to.p_ndi_name = m_sourceName.constData();
        create.source_to_connect_to.p_url_address =
            m_sourceUrl.isEmpty() ? nullptr : m_sourceUrl.constData();
        create.color_format = 3;
        create.bandwidth = 100;
        create.allow_video_fields = false;
        create.p_ndi_recv_name = "OpenLiveReplay runtime smoke receiver";
        m_recv = m_recvCreate(&create);
        return m_recv != nullptr;
    }

    bool captureVideoAndAudio(int timeoutMs, int expectedWidth, int expectedHeight,
                              FrameRate expectedRate, int expectedSampleRate, int expectedChannels,
                              QString* error) {
        CaptureStats stats;

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs && (stats.videoFrames == 0 || stats.audioFrames == 0)) {
            if (!captureOne(250, expectedWidth, expectedHeight, expectedRate, expectedSampleRate,
                            expectedChannels, &stats, error)) {
                return false;
            }
        }

        if (stats.videoFrames == 0 || stats.audioFrames == 0) {
            *error = QStringLiteral("timed out waiting for NDI frames: video=%1 audio=%2")
                         .arg(stats.videoFrames > 0)
                         .arg(stats.audioFrames > 0);
            return false;
        }
        if (!stats.sawNonSilentAudio) {
            *error = QStringLiteral("captured NDI audio frames were silent");
            return false;
        }
        return true;
    }

    bool captureOne(int timeoutMs, int expectedWidth, int expectedHeight, FrameRate expectedRate,
                    int expectedSampleRate, int expectedChannels, CaptureStats* stats,
                    QString* error) {
        NDIlib_video_frame_v2_t video;
        NDIlib_audio_frame_v3_t audio;
        const int frameType =
            m_recvCapture(m_recv, &video, &audio, nullptr, quint32(qMax(0, timeoutMs)));

        switch (frameType) {
        case FrameTypeVideo:
            if (!validateVideo(video, expectedWidth, expectedHeight, expectedRate, error)) {
                m_recvFreeVideo(m_recv, &video);
                return false;
            }
            m_recvFreeVideo(m_recv, &video);
            stats->videoFrames++;
            return true;
        case FrameTypeAudio:
            if (!validateAudio(audio, expectedSampleRate, expectedChannels, error)) {
                m_recvFreeAudio(m_recv, &audio);
                return false;
            }
            if (audioHasSignal(audio)) stats->sawNonSilentAudio = true;
            m_recvFreeAudio(m_recv, &audio);
            stats->audioFrames++;
            return true;
        case FrameTypeError:
            *error = QStringLiteral("NDI receiver returned frame_type_error");
            return false;
        case FrameTypeNone:
        case FrameTypeMetadata:
        case FrameTypeStatusChange:
        default:
            return true;
        }
    }

    void close() {
        closeReceiver();
        closeFinder();
        if (m_initialized && m_destroy) m_destroy();
        m_initialized = false;
    }

private:
    bool resolveSymbols() {
        m_initialize =
            reinterpret_cast<NDIlib_initialize_fn>(m_library.resolve("NDIlib_initialize"));
        m_destroy = reinterpret_cast<NDIlib_destroy_fn>(m_library.resolve("NDIlib_destroy"));
        m_findCreate =
            reinterpret_cast<NDIlib_find_create_v2_fn>(m_library.resolve("NDIlib_find_create_v2"));
        m_findDestroy =
            reinterpret_cast<NDIlib_find_destroy_fn>(m_library.resolve("NDIlib_find_destroy"));
        m_findWaitForSources = reinterpret_cast<NDIlib_find_wait_for_sources_fn>(
            m_library.resolve("NDIlib_find_wait_for_sources"));
        m_findGetCurrentSources = reinterpret_cast<NDIlib_find_get_current_sources_fn>(
            m_library.resolve("NDIlib_find_get_current_sources"));
        m_recvCreate =
            reinterpret_cast<NDIlib_recv_create_v3_fn>(m_library.resolve("NDIlib_recv_create_v3"));
        m_recvDestroy =
            reinterpret_cast<NDIlib_recv_destroy_fn>(m_library.resolve("NDIlib_recv_destroy"));
        m_recvCapture = reinterpret_cast<NDIlib_recv_capture_v3_fn>(
            m_library.resolve("NDIlib_recv_capture_v3"));
        m_recvFreeVideo = reinterpret_cast<NDIlib_recv_free_video_v2_fn>(
            m_library.resolve("NDIlib_recv_free_video_v2"));
        m_recvFreeAudio = reinterpret_cast<NDIlib_recv_free_audio_v3_fn>(
            m_library.resolve("NDIlib_recv_free_audio_v3"));
        return hasRequiredSymbols();
    }

    static bool validateVideo(const NDIlib_video_frame_v2_t& video, int expectedWidth,
                              int expectedHeight, FrameRate expectedRate, QString* error) {
        if (!video.p_data) {
            *error = QStringLiteral("received NDI video frame without data");
            return false;
        }
        if (video.xres != expectedWidth || video.yres != expectedHeight) {
            *error = QStringLiteral("received NDI video size %1x%2, expected %3x%4")
                         .arg(video.xres)
                         .arg(video.yres)
                         .arg(expectedWidth)
                         .arg(expectedHeight);
            return false;
        }
        if (video.line_stride_in_bytes <= 0) {
            *error = QStringLiteral("received NDI video frame without a positive stride");
            return false;
        }
        if (video.frame_rate_N != expectedRate.numerator ||
            video.frame_rate_D != expectedRate.denominator) {
            *error = QStringLiteral("received NDI video rate %1/%2, expected %3/%4")
                         .arg(video.frame_rate_N)
                         .arg(video.frame_rate_D)
                         .arg(expectedRate.numerator)
                         .arg(expectedRate.denominator);
            return false;
        }
        return true;
    }

    static bool validateAudio(const NDIlib_audio_frame_v3_t& audio, int expectedSampleRate,
                              int expectedChannels, QString* error) {
        if (!audio.p_data) {
            *error = QStringLiteral("received NDI audio frame without data");
            return false;
        }
        if (audio.sample_rate != expectedSampleRate || audio.no_channels != expectedChannels) {
            *error = QStringLiteral("received NDI audio format %1 Hz/%2 ch, expected %3 Hz/%4 ch")
                         .arg(audio.sample_rate)
                         .arg(audio.no_channels)
                         .arg(expectedSampleRate)
                         .arg(expectedChannels);
            return false;
        }
        if (audio.no_samples <= 0 || audio.channel_stride_in_bytes <= 0) {
            *error = QStringLiteral("received empty NDI audio frame");
            return false;
        }
        return true;
    }

    static bool audioHasSignal(const NDIlib_audio_frame_v3_t& audio) {
        if (!audio.p_data || audio.no_samples <= 0 || audio.no_channels <= 0 ||
            audio.channel_stride_in_bytes < int(sizeof(float))) {
            return false;
        }

        const auto* base = reinterpret_cast<const char*>(audio.p_data);
        const int samplesToScan = qMin(audio.no_samples, 256);
        for (int channel = 0; channel < audio.no_channels; ++channel) {
            const auto* samples =
                reinterpret_cast<const float*>(base + channel * audio.channel_stride_in_bytes);
            for (int sample = 0; sample < samplesToScan; ++sample) {
                if (std::abs(samples[sample]) > 0.0001f) return true;
            }
        }
        return false;
    }

    void closeReceiver() {
        if (m_recv && m_recvDestroy) m_recvDestroy(m_recv);
        m_recv = nullptr;
    }

    void closeFinder() {
        if (m_find && m_findDestroy) m_findDestroy(m_find);
        m_find = nullptr;
    }

    QLibrary m_library;
    QString m_loadedPath;
    bool m_initialized = false;
    NDIlib_find_instance_t m_find = nullptr;
    NDIlib_recv_instance_t m_recv = nullptr;
    QByteArray m_sourceName;
    QByteArray m_sourceUrl;

    NDIlib_initialize_fn m_initialize = nullptr;
    NDIlib_destroy_fn m_destroy = nullptr;
    NDIlib_find_create_v2_fn m_findCreate = nullptr;
    NDIlib_find_destroy_fn m_findDestroy = nullptr;
    NDIlib_find_wait_for_sources_fn m_findWaitForSources = nullptr;
    NDIlib_find_get_current_sources_fn m_findGetCurrentSources = nullptr;
    NDIlib_recv_create_v3_fn m_recvCreate = nullptr;
    NDIlib_recv_destroy_fn m_recvDestroy = nullptr;
    NDIlib_recv_capture_v3_fn m_recvCapture = nullptr;
    NDIlib_recv_free_video_v2_fn m_recvFreeVideo = nullptr;
    NDIlib_recv_free_audio_v3_fn m_recvFreeAudio = nullptr;
};

} // namespace

class TestNdiRuntimeSmoke : public QObject {
    Q_OBJECT
private slots:
    void realRuntimeDeliversVideoAndAudio();
};

void TestNdiRuntimeSmoke::realRuntimeDeliversVideoAndAudio() {
    NdiRuntimeReceiver receiver;
    if (!receiver.load())
        QSKIP("NDI runtime is not installed or discoverable. Set OLR_NDI_RUNTIME_LIBRARY to libndi "
              "to run this.");
    if (!receiver.hasRequiredSymbols())
        QSKIP("NDI runtime is missing finder/receiver symbols required by this smoke test.");

    const QString senderName = QStringLiteral("OLR NDI Runtime Smoke %1 %2")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch());

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("ndi-runtime-smoke");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), senderName);

    const FrameRate outputRate = FrameRate::fromFraction(25, 1);
    NdiOutputSink sink;
    OutputDispatcher dispatcher(outputRate, 1, 16, 16);
    dispatcher.setEndpoints({{assignment, &sink}});
    if (!sink.isActive()) {
        QFAIL(qPrintable(QStringLiteral("failed to start NDI sender through app sink: %1")
                             .arg(sink.status().message)));
    }

    QVERIFY2(receiver.createFinder(), "failed to create NDI finder");

    OutputFrameCache cache(1, 16, 16);
    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    for (int i = 0; i < 5; ++i) {
        insertSourceFrame(&cache, i);
        dispatcher.dispatchTick(cache, state);
        QVERIFY2(
            sink.status().state == NdiOutputState::Active,
            qPrintable(
                QStringLiteral("NDI sink failed during warmup: %1").arg(sink.status().message)));
        QThread::msleep(40);
    }

    QVERIFY2(receiver.waitForSource(senderName, 10000),
             qPrintable(QStringLiteral("NDI finder did not discover sender '%1' via %2")
                            .arg(senderName, receiver.loadedPath())));
    QVERIFY2(receiver.createReceiver(), "failed to create NDI receiver");

    for (int i = 5; i < 35; ++i) {
        insertSourceFrame(&cache, i);
        dispatcher.dispatchTick(cache, state);
        QVERIFY2(
            sink.status().state == NdiOutputState::Active,
            qPrintable(
                QStringLiteral("NDI sink failed before capture: %1").arg(sink.status().message)));
        QThread::msleep(40);
    }

    QString error;
    QVERIFY2(receiver.captureVideoAndAudio(10000, 16, 16, outputRate, 48000, 2, &error),
             qPrintable(QStringLiteral("NDI receiver did not capture app output: %1").arg(error)));

    bool ok = false;
    const int soakSeconds = qEnvironmentVariableIntValue("OLR_NDI_RUNTIME_SOAK_SECONDS", &ok);
    if (ok && soakSeconds > 0) {
        CaptureStats stats;
        QElapsedTimer soakTimer;
        soakTimer.start();
        qint64 frameIndex = 35;
        qint64 nextSendMs = 0;
        const qint64 submittedBeforeSoak = dispatcher.stats().framesSubmitted;

        while (soakTimer.elapsed() < qint64(soakSeconds) * 1000) {
            if (soakTimer.elapsed() >= nextSendMs) {
                insertSourceFrame(&cache, frameIndex);
                dispatcher.dispatchTick(cache, state);
                QVERIFY2(sink.status().state == NdiOutputState::Active,
                         qPrintable(QStringLiteral("NDI sink failed during soak: %1")
                                        .arg(sink.status().message)));
                ++frameIndex;
                nextSendMs = soakTimer.elapsed() + 40;
            }

            QVERIFY2(receiver.captureOne(5, 16, 16, outputRate, 48000, 2, &stats, &error),
                     qPrintable(QStringLiteral("NDI receiver failed during soak: %1").arg(error)));
            QCoreApplication::processEvents();
        }

        const qint64 drainUntilMs = soakTimer.elapsed() + 1000;
        while (soakTimer.elapsed() < drainUntilMs) {
            QVERIFY2(receiver.captureOne(5, 16, 16, outputRate, 48000, 2, &stats, &error),
                     qPrintable(
                         QStringLiteral("NDI receiver failed while draining soak: %1").arg(error)));
        }

        const qint64 submittedDuringSoak = dispatcher.stats().framesSubmitted - submittedBeforeSoak;
        const qint64 minimumCapturedFrames = submittedDuringSoak * 9 / 10;

        qInfo("NDI runtime soak submitted %lld frames and captured %d video/%d audio frames over "
              "%d seconds",
              static_cast<long long>(submittedDuringSoak), stats.videoFrames, stats.audioFrames,
              soakSeconds);
        QVERIFY2(stats.videoFrames >= minimumCapturedFrames,
                 qPrintable(QStringLiteral("NDI soak captured too few video frames: %1 of %2")
                                .arg(stats.videoFrames)
                                .arg(submittedDuringSoak)));
        QVERIFY2(stats.audioFrames >= minimumCapturedFrames,
                 qPrintable(QStringLiteral("NDI soak captured too few audio frames: %1 of %2")
                                .arg(stats.audioFrames)
                                .arg(submittedDuringSoak)));
        QVERIFY2(stats.sawNonSilentAudio, "NDI soak captured only silent audio");
    }
}

QTEST_GUILESS_MAIN(TestNdiRuntimeSmoke)
#include "tst_ndi_runtime_smoke.moc"
