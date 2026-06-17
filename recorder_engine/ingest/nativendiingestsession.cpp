#include "nativendiingestsession.h"

#include <QByteArray>
#include <QDir>
#include <QLibrary>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr int kCaptureTimeoutMs = 100;
constexpr qint64 kNdiTimecodeSynthesize = std::numeric_limits<qint64>::max();
constexpr int kNdiRecvColorFastest = 100;

struct NDIlib_source_t {
    const char* p_ndi_name = nullptr;
    const char* p_url_address = nullptr;
};

struct NDIlib_recv_create_v3_t {
    NDIlib_source_t source_to_connect_to;
    int color_format = 0;
    int bandwidth = 0;
    bool allow_video_fields = false;
    const char* p_ndi_recv_name = nullptr;
};

struct NDIlib_video_frame_v2_t {
    int xres = 0;
    int yres = 0;
    uint32_t FourCC = 0;
    int frame_rate_N = 0;
    int frame_rate_D = 1;
    float picture_aspect_ratio = 0.0f;
    int frame_format_type = 1;
    qint64 timecode = kNdiTimecodeSynthesize;
    uint8_t* p_data = nullptr;
    int line_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    qint64 timestamp = 0;
};

struct NDIlib_audio_frame_v3_t {
    int sample_rate = 48000;
    int no_channels = 2;
    int no_samples = 0;
    qint64 timecode = kNdiTimecodeSynthesize;
    uint32_t FourCC = 0;
    uint8_t* p_data = nullptr;
    int channel_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    qint64 timestamp = 0;
};

struct NDIlib_metadata_frame_t {
    int length = 0;
    qint64 timecode = kNdiTimecodeSynthesize;
    char* p_data = nullptr;
};

enum NDIlib_frame_type_e {
    NDIlib_frame_type_none = 0,
    NDIlib_frame_type_video = 1,
    NDIlib_frame_type_audio = 2,
    NDIlib_frame_type_metadata = 3,
    NDIlib_frame_type_error = 4,
};

using NDIlib_find_instance_t = void*;
using NDIlib_recv_instance_t = void*;
using NDIlib_initialize_fn = bool (*)();
using NDIlib_destroy_fn = void (*)();
using NDIlib_find_create_v2_fn = NDIlib_find_instance_t (*)(const void*);
using NDIlib_find_destroy_fn = void (*)(NDIlib_find_instance_t);
using NDIlib_find_wait_for_sources_fn = bool (*)(NDIlib_find_instance_t, uint32_t);
using NDIlib_find_get_current_sources_fn = const NDIlib_source_t* (*)(NDIlib_find_instance_t,
                                                                     uint32_t*);
using NDIlib_recv_create_v3_fn = NDIlib_recv_instance_t (*)(const NDIlib_recv_create_v3_t*);
using NDIlib_recv_destroy_fn = void (*)(NDIlib_recv_instance_t);
using NDIlib_recv_capture_v3_fn = NDIlib_frame_type_e (*)(NDIlib_recv_instance_t,
                                                          NDIlib_video_frame_v2_t*,
                                                          NDIlib_audio_frame_v3_t*,
                                                          NDIlib_metadata_frame_t*, uint32_t);
using NDIlib_recv_free_video_v2_fn = void (*)(NDIlib_recv_instance_t,
                                              const NDIlib_video_frame_v2_t*);
using NDIlib_recv_free_audio_v3_fn = void (*)(NDIlib_recv_instance_t,
                                              const NDIlib_audio_frame_v3_t*);

QStringList ndiRuntimeLibraryCandidates() {
    QStringList candidates;
    const QByteArray explicitPath = qgetenv("OLR_NDI_RUNTIME_LIBRARY");
    if (!explicitPath.isEmpty()) candidates.append(QString::fromLocal8Bit(explicitPath));
#if defined(Q_OS_WIN)
    const QString runtimeDir = QString::fromLocal8Bit(qgetenv("NDI_RUNTIME_DIR_V6"));
    if (!runtimeDir.isEmpty())
        candidates.append(QDir(runtimeDir).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll")));
    candidates.append(QStringLiteral("Processing.NDI.Lib.x64.dll"));
#elif defined(Q_OS_MACOS)
    candidates.append(QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib"));
    candidates.append(QStringLiteral("/usr/local/lib/libndi.dylib"));
    candidates.append(QStringLiteral("libndi.dylib"));
#else
    candidates.append(QStringLiteral("libndi.so"));
#endif
    candidates.removeDuplicates();
    return candidates;
}

int selectDiscoveredSourceIndex(const QStringList& discovered, const QString& wanted) {
    for (int i = 0; i < discovered.size(); ++i) {
        if (discovered.at(i) == wanted) {
            return i;
        }
    }
    for (int i = 0; i < discovered.size(); ++i) {
        if (discovered.at(i).contains(wanted, Qt::CaseInsensitive)) {
            return i;
        }
    }
    return -1;
}

class NdiDynamicReceiverBackend final : public INdiReceiverBackend {
public:
    ~NdiDynamicReceiverBackend() override {
        closeReceiver();
        if (m_destroy && m_initialized) m_destroy();
    }

    bool isRuntimeAvailable() const override {
        return const_cast<NdiDynamicReceiverBackend*>(this)->ensureLoaded();
    }

    bool openReceiver(const QString& sourceName) override {
        closeReceiver();
        if (sourceName.trimmed().isEmpty() || !ensureLoaded() || !m_findCreate || !m_recvCreate) {
            return false;
        }

        NDIlib_find_instance_t finder = m_findCreate(nullptr);
        if (!finder) {
            return false;
        }
        if (m_findWaitForSources) {
            m_findWaitForSources(finder, 1000);
        }
        uint32_t count = 0;
        const NDIlib_source_t* sources = m_findGetCurrentSources
                                            ? m_findGetCurrentSources(finder, &count)
                                            : nullptr;
        QByteArray wanted = sourceName.toUtf8();
        NDIlib_source_t selected;
        QStringList discovered;
        for (uint32_t i = 0; sources && i < count; ++i) {
            discovered.append(QString::fromUtf8(sources[i].p_ndi_name ? sources[i].p_ndi_name : ""));
        }
        const int selectedIndex = selectDiscoveredSourceIndex(discovered, sourceName);
        if (selectedIndex >= 0) {
            selected = sources[selectedIndex];
        } else {
            selected.p_ndi_name = wanted.constData();
            selected.p_url_address = nullptr;
        }

        NDIlib_recv_create_v3_t create;
        create.source_to_connect_to = selected;
        create.color_format = kNdiRecvColorFastest;
        create.bandwidth = 0;
        create.allow_video_fields = false;
        m_receiver = m_recvCreate(&create);
        m_findDestroy(finder);
        return m_receiver != nullptr;
    }

    void closeReceiver() override {
        if (m_receiver && m_recvDestroy) {
            m_recvDestroy(m_receiver);
        }
        m_receiver = nullptr;
    }

    Capture capture(NdiVideoFrame* video, NdiAudioFrame* audio, int timeoutMs) override {
        if (!m_receiver || !m_recvCapture) {
            return Capture::Error;
        }
        std::memset(&m_lastVideo, 0, sizeof(m_lastVideo));
        std::memset(&m_lastAudio, 0, sizeof(m_lastAudio));
        const NDIlib_frame_type_e type =
            m_recvCapture(m_receiver, &m_lastVideo, &m_lastAudio, nullptr, uint32_t(timeoutMs));
        if (type == NDIlib_frame_type_video && video) {
            video->width = m_lastVideo.xres;
            video->height = m_lastVideo.yres;
            video->strideBytes = m_lastVideo.line_stride_in_bytes;
            video->fourCc = m_lastVideo.FourCC;
            video->data = m_lastVideo.p_data;
            video->timestamp100ns = m_lastVideo.timestamp;
            video->timecode100ns = m_lastVideo.timecode;
            return Capture::Video;
        }
        if (type == NDIlib_frame_type_audio && audio) {
            audio->sampleRate = m_lastAudio.sample_rate;
            audio->channels = m_lastAudio.no_channels;
            audio->samples = m_lastAudio.no_samples;
            audio->channelStrideBytes = m_lastAudio.channel_stride_in_bytes;
            audio->data = reinterpret_cast<const float*>(m_lastAudio.p_data);
            audio->timestamp100ns = m_lastAudio.timestamp;
            audio->timecode100ns = m_lastAudio.timecode;
            return Capture::Audio;
        }
        if (type == NDIlib_frame_type_error) {
            return Capture::Error;
        }
        return Capture::None;
    }

    void freeVideo(NdiVideoFrame*) override {
        if (m_receiver && m_recvFreeVideo && m_lastVideo.p_data) {
            m_recvFreeVideo(m_receiver, &m_lastVideo);
            m_lastVideo.p_data = nullptr;
        }
    }

    void freeAudio(NdiAudioFrame*) override {
        if (m_receiver && m_recvFreeAudio && m_lastAudio.p_data) {
            m_recvFreeAudio(m_receiver, &m_lastAudio);
            m_lastAudio.p_data = nullptr;
        }
    }

private:
    bool ensureLoaded() {
        if (m_loaded) return true;

        for (const QString& candidate : ndiRuntimeLibraryCandidates()) {
            m_library.setFileName(candidate);
            if (!m_library.load()) continue;
            if (resolveSymbols()) {
                m_initialized = !m_initialize || m_initialize();
                m_loaded = m_initialized;
                if (m_loaded) return true;
            }
            m_library.unload();
        }
        return false;
    }

    bool resolveSymbols() {
        m_initialize =
            reinterpret_cast<NDIlib_initialize_fn>(m_library.resolve("NDIlib_initialize"));
        m_destroy = reinterpret_cast<NDIlib_destroy_fn>(m_library.resolve("NDIlib_destroy"));
        m_findCreate = reinterpret_cast<NDIlib_find_create_v2_fn>(
            m_library.resolve("NDIlib_find_create_v2"));
        m_findDestroy =
            reinterpret_cast<NDIlib_find_destroy_fn>(m_library.resolve("NDIlib_find_destroy"));
        m_findWaitForSources = reinterpret_cast<NDIlib_find_wait_for_sources_fn>(
            m_library.resolve("NDIlib_find_wait_for_sources"));
        m_findGetCurrentSources = reinterpret_cast<NDIlib_find_get_current_sources_fn>(
            m_library.resolve("NDIlib_find_get_current_sources"));
        m_recvCreate = reinterpret_cast<NDIlib_recv_create_v3_fn>(
            m_library.resolve("NDIlib_recv_create_v3"));
        m_recvDestroy =
            reinterpret_cast<NDIlib_recv_destroy_fn>(m_library.resolve("NDIlib_recv_destroy"));
        m_recvCapture = reinterpret_cast<NDIlib_recv_capture_v3_fn>(
            m_library.resolve("NDIlib_recv_capture_v3"));
        m_recvFreeVideo = reinterpret_cast<NDIlib_recv_free_video_v2_fn>(
            m_library.resolve("NDIlib_recv_free_video_v2"));
        m_recvFreeAudio = reinterpret_cast<NDIlib_recv_free_audio_v3_fn>(
            m_library.resolve("NDIlib_recv_free_audio_v3"));
        return m_findCreate && m_findDestroy && m_findGetCurrentSources && m_recvCreate &&
               m_recvDestroy && m_recvCapture && m_recvFreeVideo && m_recvFreeAudio;
    }

    QLibrary m_library;
    bool m_loaded = false;
    bool m_initialized = false;
    NDIlib_recv_instance_t m_receiver = nullptr;
    NDIlib_video_frame_v2_t m_lastVideo;
    NDIlib_audio_frame_v3_t m_lastAudio;

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

NativeNdiIngestSession::NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning)
    : NativeNdiIngestSession(sourceIndex, outputWidth, outputHeight, captureRunning, nullptr,
                             nullptr) {
    m_ownedBackend = std::make_unique<NdiDynamicReceiverBackend>();
    m_backend = m_ownedBackend.get();
}

NativeNdiIngestSession::NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning,
                                               AnchoredSourceClock* sourceClock)
    : NativeNdiIngestSession(sourceIndex, outputWidth, outputHeight, captureRunning, nullptr,
                             sourceClock) {
    m_ownedBackend = std::make_unique<NdiDynamicReceiverBackend>();
    m_backend = m_ownedBackend.get();
}

NativeNdiIngestSession::NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning,
                                               INdiReceiverBackend* backend)
    : NativeNdiIngestSession(sourceIndex, outputWidth, outputHeight, captureRunning, backend,
                             nullptr) {}

NativeNdiIngestSession::NativeNdiIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning,
                                               INdiReceiverBackend* backend,
                                               AnchoredSourceClock* sourceClock)
    : m_outputWidth(outputWidth)
    , m_outputHeight(outputHeight)
    , m_captureRunning(captureRunning)
    , m_backend(backend)
    , m_clock(sourceClock ? sourceClock : &m_ownedClock)
    , m_externalClock(sourceClock != nullptr) {
    Q_UNUSED(sourceIndex);
    m_monotonic.start();
}

NativeNdiIngestSession::~NativeNdiIngestSession() {
    requestStop();
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
}

bool NativeNdiIngestSession::supportsUrl(const QUrl& url) {
    return !sourceNameFromUrl(url).isEmpty();
}

bool NativeNdiIngestSession::runtimeAvailable() {
    NdiDynamicReceiverBackend backend;
    return backend.isRuntimeAvailable();
}

QString NativeNdiIngestSession::sourceNameFromUrl(const QUrl& url) {
    if (url.scheme().toLower() != QStringLiteral("ndi")) {
        return {};
    }
    QByteArray encoded = url.authority(QUrl::FullyEncoded).toUtf8();
    if (encoded.isEmpty()) {
        QString path = url.path(QUrl::FullyEncoded);
        if (path.startsWith(QLatin1Char('/'))) {
            path.remove(0, 1);
        }
        encoded = path.toUtf8();
    }
    return QUrl::fromPercentEncoding(encoded).trimmed();
}

bool NativeNdiIngestSession::recvCreateSourceIsValueForTest() {
    return std::is_same<decltype(NDIlib_recv_create_v3_t::source_to_connect_to),
                        NDIlib_source_t>::value;
}

QStringList NativeNdiIngestSession::runtimeLibraryCandidatesForTest() {
    return ndiRuntimeLibraryCandidates();
}

QString NativeNdiIngestSession::selectDiscoveredSourceForTest(const QStringList& discovered,
                                                              const QString& wanted) {
    const int index = selectDiscoveredSourceIndex(discovered, wanted);
    return index >= 0 ? discovered.at(index) : QString();
}

bool NativeNdiIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    m_callbacks = callbacks;
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_lastFailureKind = IngestFailureKind::None;
    m_lastStatsAtMs = -1;
    if (!m_externalClock) {
        m_clock->reset();
    }
    if (!m_backend || !m_backend->isRuntimeAvailable()) {
        m_lastFailureKind = IngestFailureKind::DecodeCapability;
        return false;
    }
    const QString sourceName = sourceNameFromUrl(url);
    if (sourceName.isEmpty() || !m_backend->openReceiver(sourceName)) {
        m_lastFailureKind = IngestFailureKind::TransientNetwork;
        return false;
    }
    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(true);
    }
    return true;
}

void NativeNdiIngestSession::run() {
    if (!m_backend) {
        return;
    }
    while (!shouldStop()) {
        NdiVideoFrame video;
        NdiAudioFrame audio;
        const INdiReceiverBackend::Capture result =
            m_backend->capture(&video, &audio, kCaptureTimeoutMs);
        if (result == INdiReceiverBackend::Capture::Video) {
            AVFrame* frame = ndiVideoToYuv420p(video, m_outputWidth, m_outputHeight, &m_sws);
            const int64_t sourcePtsMs =
                mapTimestampMs(video.timestamp100ns, ClockObservationRole::Authority);
            maybeReportStats();
            if (frame && sourcePtsMs >= 0 && m_callbacks.onVideoFrame) {
                DecodedVideoFrame decoded;
                decoded.frame = frame;
                decoded.sourcePtsMs = sourcePtsMs;
                decoded.sourceTimecode100ns =
                    video.timecode100ns == kNdiTimecodeSynthesize ? -1 : video.timecode100ns;
                m_callbacks.onVideoFrame(decoded);
            } else if (frame) {
                av_frame_free(&frame);
            }
            m_backend->freeVideo(&video);
        } else if (result == INdiReceiverBackend::Capture::Audio) {
            const QByteArray pcm = ndiAudioToS16Stereo(audio);
            const int64_t sourcePtsMs =
                mapTimestampMs(audio.timestamp100ns, ClockObservationRole::Follower);
            maybeReportStats();
            if (!pcm.isEmpty() && sourcePtsMs >= 0 && m_callbacks.onAudioChunk) {
                DecodedAudioChunk chunk;
                chunk.startSample = sourcePtsMs * 48000 / 1000;
                chunk.sourceTimecode100ns =
                    audio.timecode100ns == kNdiTimecodeSynthesize ? -1 : audio.timecode100ns;
                chunk.pcmS16Stereo = pcm;
                m_callbacks.onAudioChunk(std::move(chunk));
            }
            m_backend->freeAudio(&audio);
        } else if (result == INdiReceiverBackend::Capture::Error) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            break;
        } else {
            QThread::msleep(1);
        }
    }
    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(false);
    }
}

void NativeNdiIngestSession::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_backend) {
        m_backend->closeReceiver();
    }
}

bool NativeNdiIngestSession::shouldStop() const {
    if (m_stopRequested.load(std::memory_order_relaxed)) {
        return true;
    }
    if (m_captureRunning && !m_captureRunning->load(std::memory_order_relaxed)) {
        return true;
    }
    return m_callbacks.shouldStop ? m_callbacks.shouldStop() : false;
}

int64_t NativeNdiIngestSession::mapTimestampMs(int64_t timestamp100ns,
                                               ClockObservationRole role) {
    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    if (timestamp100ns < 0 || timestamp100ns == std::numeric_limits<int64_t>::max()) {
        return nowMs;
    }
    if (nowMs < 0) {
        return -1;
    }
    m_clock->observe(timestamp100ns, nowMs, false, role);
    return m_clock->toSessionMs(timestamp100ns);
}

void NativeNdiIngestSession::maybeReportStats() {
    if (!m_callbacks.reportStats) {
        return;
    }
    const int64_t now = m_monotonic.isValid() ? m_monotonic.elapsed() : 0;
    if (m_lastStatsAtMs >= 0 && now - m_lastStatsAtMs < 1000) {
        return;
    }
    m_lastStatsAtMs = now;
    IngestStats stats;
    stats.kind = IngestStatsKind::Ndi;
    stats.clockPpm = m_clock->ppm();
    stats.clockQuality = int(m_clock->quality());
    m_callbacks.reportStats(stats);
}
