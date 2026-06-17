#include "playback/output/ndisink.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QLibrary>
#include <QThread>
#include <QtTest>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr quint32 ndiFourCc(char a, char b, char c, char d) {
    return quint32(quint8(a)) | (quint32(quint8(b)) << 8) | (quint32(quint8(c)) << 16) |
           (quint32(quint8(d)) << 24);
}

constexpr qint64 kNdiTimecodeSynthesize = std::numeric_limits<qint64>::max();
constexpr quint32 kNdiFourCcI420 = ndiFourCc('I', '4', '2', '0');
constexpr quint32 kNdiFourCcFltp = ndiFourCc('F', 'L', 'T', 'p');

enum NdiFrameType {
    NdiFrameTypeNone = 0,
    NdiFrameTypeVideo = 1,
    NdiFrameTypeAudio = 2,
    NdiFrameTypeMetadata = 3,
    NdiFrameTypeError = 4,
    NdiFrameTypeStatusChange = 100,
};

struct NDIlib_source_t {
    const char* p_ndi_name = nullptr;
    const char* p_url_address = nullptr;
};

struct NDIlib_find_create_t {
    bool show_local_sources = true;
    const char* p_groups = nullptr;
    const char* p_extra_ips = nullptr;
};

struct NDIlib_recv_create_v3_t {
    NDIlib_source_t source_to_connect_to;
    int color_format = 3; // NDIlib_recv_color_format_UYVY_RGBA.
    int bandwidth = 100;  // NDIlib_recv_bandwidth_highest.
    bool allow_video_fields = false;
    const char* p_ndi_recv_name = nullptr;
};

struct NDIlib_video_frame_v2_t {
    int xres = 0;
    int yres = 0;
    quint32 FourCC = kNdiFourCcI420;
    int frame_rate_N = 0;
    int frame_rate_D = 1;
    float picture_aspect_ratio = 0.0f;
    int frame_format_type = 1;
    qint64 timecode = kNdiTimecodeSynthesize;
    quint8* p_data = nullptr;
    int line_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    qint64 timestamp = 0;
};

struct NDIlib_audio_frame_v3_t {
    int sample_rate = 48000;
    int no_channels = 2;
    int no_samples = 0;
    qint64 timecode = kNdiTimecodeSynthesize;
    quint32 FourCC = kNdiFourCcFltp;
    quint8* p_data = nullptr;
    int channel_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    qint64 timestamp = 0;
};

using NDIlib_find_instance_t = void*;
using NDIlib_recv_instance_t = void*;
using NDIlib_initialize_fn = bool (*)();
using NDIlib_find_create_v2_fn = NDIlib_find_instance_t (*)(const NDIlib_find_create_t*);
using NDIlib_find_destroy_fn = void (*)(NDIlib_find_instance_t);
using NDIlib_find_wait_for_sources_fn = bool (*)(NDIlib_find_instance_t, quint32);
using NDIlib_find_get_current_sources_fn = const NDIlib_source_t* (*) (NDIlib_find_instance_t,
                                                                       quint32*);
using NDIlib_recv_create_v3_fn = NDIlib_recv_instance_t (*)(const NDIlib_recv_create_v3_t*);
using NDIlib_recv_destroy_fn = void (*)(NDIlib_recv_instance_t);
using NDIlib_recv_capture_v3_fn = int (*)(NDIlib_recv_instance_t, NDIlib_video_frame_v2_t*,
                                          NDIlib_audio_frame_v3_t*, void*, quint32);
using NDIlib_recv_free_video_v2_fn = void (*)(NDIlib_recv_instance_t,
                                              const NDIlib_video_frame_v2_t*);
using NDIlib_recv_free_audio_v3_fn = void (*)(NDIlib_recv_instance_t,
                                              const NDIlib_audio_frame_v3_t*);
using NDIlib_destroy_fn = void (*)();

struct CaptureStats {
    int videoFrames = 0;
    int audioFrames = 0;
};

QStringList ndiRuntimeCandidates() {
    QStringList candidates;
    const QByteArray explicitPath = qgetenv("OLR_NDI_RUNTIME_LIBRARY");
    if (!explicitPath.isEmpty()) candidates.append(QString::fromLocal8Bit(explicitPath));

#if defined(Q_OS_WIN)
    const QString runtimeDir = QString::fromLocal8Bit(qgetenv("NDI_RUNTIME_DIR_V6"));
    if (!runtimeDir.isEmpty())
        candidates.append(QDir(runtimeDir).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll")));
    candidates.append(QStringLiteral("Processing.NDI.Lib.x64.dll"));
#elif defined(Q_OS_MACOS)
    candidates.append(QStringLiteral("/usr/local/lib/libndi.dylib"));
    candidates.append(QStringLiteral("libndi.dylib"));
#else
    candidates.append(QStringLiteral("libndi.so"));
#endif
    return candidates;
}

OutputBusFrame makeFrame(qint64 index, uchar y) {
    OutputBusFrame frame;
    frame.bus = OutputBusId::feed(0);
    frame.outputFrameIndex = index;
    frame.sampledPlayheadMs = index * 40;
    frame.video = MediaVideoFrame::solidYuv420p(16, 16, y, 96, 160);
    frame.video.feedIndex = 0;
    frame.video.ptsMs = frame.sampledPlayheadMs;
    frame.video.outputFrameIndex = index;

    frame.audio.feedIndex = 0;
    frame.audio.sampleRate = 48000;
    frame.audio.channels = 2;
    frame.audio.format = MediaSampleFormat::S16Interleaved;
    const int sampleFrames = 1920;
    frame.audio.pcm.resize(sampleFrames * frame.audio.channels * int(sizeof(qint16)));
    auto* out = reinterpret_cast<qint16*>(frame.audio.pcm.data());
    for (int sample = 0; sample < sampleFrames; ++sample) {
        const auto value = qint16(std::lround(std::sin(double(sample) * 0.05) * 12000.0));
        out[sample * 2] = value;
        out[sample * 2 + 1] = qint16(-value);
    }
    frame.identity = outputFrameIdentityFor(frame);
    return frame;
}

class NdiRuntimeReceiver final {
public:
    ~NdiRuntimeReceiver() { close(); }

    bool load() {
        for (const QString& candidate : ndiRuntimeCandidates()) {
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
                              int expectedSampleRate, int expectedChannels, QString* error) {
        CaptureStats stats;

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs && (stats.videoFrames == 0 || stats.audioFrames == 0)) {
            if (!captureOne(250, expectedWidth, expectedHeight, expectedSampleRate,
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
        return true;
    }

    bool captureOne(int timeoutMs, int expectedWidth, int expectedHeight, int expectedSampleRate,
                    int expectedChannels, CaptureStats* stats, QString* error) {
        NDIlib_video_frame_v2_t video;
        NDIlib_audio_frame_v3_t audio;
        const int frameType =
            m_recvCapture(m_recv, &video, &audio, nullptr, quint32(qMax(0, timeoutMs)));

        switch (frameType) {
        case NdiFrameTypeVideo:
            if (!validateVideo(video, expectedWidth, expectedHeight, error)) {
                m_recvFreeVideo(m_recv, &video);
                return false;
            }
            m_recvFreeVideo(m_recv, &video);
            stats->videoFrames++;
            return true;
        case NdiFrameTypeAudio:
            if (!validateAudio(audio, expectedSampleRate, expectedChannels, error)) {
                m_recvFreeAudio(m_recv, &audio);
                return false;
            }
            m_recvFreeAudio(m_recv, &audio);
            stats->audioFrames++;
            return true;
        case NdiFrameTypeError:
            *error = QStringLiteral("NDI receiver returned frame_type_error");
            return false;
        case NdiFrameTypeNone:
        case NdiFrameTypeMetadata:
        case NdiFrameTypeStatusChange:
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
                              int expectedHeight, QString* error) {
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
    if (qEnvironmentVariableIsEmpty("OLR_RUN_NDI_RUNTIME_TESTS"))
        QSKIP("Set OLR_RUN_NDI_RUNTIME_TESTS=1 to run the real NDI runtime smoke test.");

    NdiRuntimeReceiver receiver;
    if (!receiver.load())
        QSKIP("NDI runtime is not installed. Set OLR_NDI_RUNTIME_LIBRARY to libndi to run this.");
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

    NdiOutputSink sink;
    if (!sink.start(assignment, FrameRate::fromFraction(25, 1))) {
        QFAIL(qPrintable(QStringLiteral("failed to start NDI sender through app sink: %1")
                             .arg(sink.status().message)));
    }

    QVERIFY2(receiver.createFinder(), "failed to create NDI finder");

    for (int i = 0; i < 5; ++i) {
        QVERIFY2(sink.submit(makeFrame(i, uchar(70 + i))), "failed to submit frame to NDI sink");
        QThread::msleep(40);
    }

    QVERIFY2(receiver.waitForSource(senderName, 10000),
             qPrintable(QStringLiteral("NDI finder did not discover sender '%1' via %2")
                            .arg(senderName, receiver.loadedPath())));
    QVERIFY2(receiver.createReceiver(), "failed to create NDI receiver");

    for (int i = 5; i < 35; ++i) {
        QVERIFY2(sink.submit(makeFrame(i, uchar(70 + (i % 30)))),
                 "failed to submit frame to NDI sink");
        QThread::msleep(40);
    }

    QString error;
    QVERIFY2(receiver.captureVideoAndAudio(10000, 16, 16, 48000, 2, &error),
             qPrintable(QStringLiteral("NDI receiver did not capture app output: %1").arg(error)));

    bool ok = false;
    const int soakSeconds = qEnvironmentVariableIntValue("OLR_NDI_RUNTIME_SOAK_SECONDS", &ok);
    if (ok && soakSeconds > 0) {
        CaptureStats stats;
        QElapsedTimer soakTimer;
        soakTimer.start();
        qint64 frameIndex = 35;
        qint64 nextSendMs = 0;

        while (soakTimer.elapsed() < qint64(soakSeconds) * 1000) {
            if (soakTimer.elapsed() >= nextSendMs) {
                QVERIFY2(sink.submit(makeFrame(frameIndex, uchar(70 + (frameIndex % 30)))),
                         "failed to submit frame to NDI sink during soak");
                ++frameIndex;
                nextSendMs = soakTimer.elapsed() + 40;
            }

            QVERIFY2(receiver.captureOne(5, 16, 16, 48000, 2, &stats, &error),
                     qPrintable(QStringLiteral("NDI receiver failed during soak: %1").arg(error)));
            QCoreApplication::processEvents();
        }

        const qint64 drainUntilMs = soakTimer.elapsed() + 1000;
        while (soakTimer.elapsed() < drainUntilMs) {
            QVERIFY2(receiver.captureOne(5, 16, 16, 48000, 2, &stats, &error),
                     qPrintable(
                         QStringLiteral("NDI receiver failed while draining soak: %1").arg(error)));
        }

        qInfo("NDI runtime soak captured %d video frames and %d audio frames over %d seconds",
              stats.videoFrames, stats.audioFrames, soakSeconds);
        QVERIFY2(stats.videoFrames >= soakSeconds * 5,
                 qPrintable(QStringLiteral("NDI soak captured too few video frames: %1 in %2 s")
                                .arg(stats.videoFrames)
                                .arg(soakSeconds)));
        QVERIFY2(stats.audioFrames >= soakSeconds,
                 qPrintable(QStringLiteral("NDI soak captured too few audio frames: %1 in %2 s")
                                .arg(stats.audioFrames)
                                .arg(soakSeconds)));
    }
}

QTEST_GUILESS_MAIN(TestNdiRuntimeSmoke)
#include "tst_ndi_runtime_smoke.moc"
