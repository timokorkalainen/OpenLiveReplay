#include "playback/output/ndisink.h"

#include <QByteArray>
#include <QDir>
#include <QLibrary>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

constexpr quint32 ndiFourCc(char a, char b, char c, char d) {
    return quint32(quint8(a)) | (quint32(quint8(b)) << 8) | (quint32(quint8(c)) << 16) |
           (quint32(quint8(d)) << 24);
}

constexpr quint32 kNdiFourCcI420 = ndiFourCc('I', '4', '2', '0');
constexpr quint32 kNdiFourCcFltp = ndiFourCc('F', 'L', 'T', 'p');
constexpr int kNdiFrameFormatProgressive = 1;
constexpr qint64 kNdiTimecodeSynthesize = std::numeric_limits<qint64>::max();

struct NDIlib_send_create_t {
    const char* p_ndi_name = nullptr;
    const char* p_groups = nullptr;
    bool clock_video = false;
    bool clock_audio = false;
};

struct NDIlib_video_frame_v2_t {
    int xres = 0;
    int yres = 0;
    quint32 FourCC = kNdiFourCcI420;
    int frame_rate_N = 0;
    int frame_rate_D = 1;
    float picture_aspect_ratio = 0.0f;
    int frame_format_type = kNdiFrameFormatProgressive;
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

using NDIlib_send_instance_t = void*;
using NDIlib_initialize_fn = bool (*)();
using NDIlib_destroy_fn = void (*)();
using NDIlib_send_create_fn = NDIlib_send_instance_t (*)(const NDIlib_send_create_t*);
using NDIlib_send_destroy_fn = void (*)(NDIlib_send_instance_t);
using NDIlib_send_send_video_v2_fn = void (*)(NDIlib_send_instance_t,
                                              const NDIlib_video_frame_v2_t*);
using NDIlib_send_send_audio_v3_fn = void (*)(NDIlib_send_instance_t,
                                              const NDIlib_audio_frame_v3_t*);

bool copyPlane(const QByteArray& src, int srcStride, int width, int height, char* dst,
               int dstStride) {
    if (srcStride <= 0 || width <= 0 || height <= 0 || dstStride < width) return false;
    if (src.size() < srcStride * height) return false;
    for (int y = 0; y < height; ++y) {
        memcpy(dst + y * dstStride, src.constData() + y * srcStride, size_t(width));
    }
    return true;
}

class NdiDynamicSenderBackend final : public INdiSenderBackend {
public:
    ~NdiDynamicSenderBackend() override {
        destroySender();
        if (m_destroy && m_initialized) m_destroy();
    }

    bool isRuntimeAvailable() const override {
        return const_cast<NdiDynamicSenderBackend*>(this)->ensureLoaded();
    }

    bool createSender(const QString& senderName, FrameRate rate) override {
        destroySender();
        if (!rate.isValid() || senderName.isEmpty() || !ensureLoaded()) return false;

        m_senderNameUtf8 = senderName.toUtf8();
        NDIlib_send_create_t create;
        create.p_ndi_name = m_senderNameUtf8.constData();
        create.p_groups = nullptr;
        create.clock_video = false;
        create.clock_audio = false;
        m_sender = m_sendCreate(&create);
        m_rate = rate;
        return m_sender != nullptr;
    }

    void destroySender() override {
        if (m_sender && m_sendDestroy) {
            m_sendDestroy(m_sender);
        }
        m_sender = nullptr;
    }

    bool sendFrame(const OutputBusFrame& frame) override {
        if (!m_sender || !m_sendVideo || !frame.video.isValid()) return false;
        if (!packI420(frame.video)) return false;

        NDIlib_video_frame_v2_t videoFrame;
        videoFrame.xres = frame.video.width;
        videoFrame.yres = frame.video.height;
        videoFrame.FourCC = kNdiFourCcI420;
        videoFrame.frame_rate_N = m_rate.numerator;
        videoFrame.frame_rate_D = m_rate.denominator;
        videoFrame.picture_aspect_ratio =
            frame.video.height > 0 ? float(frame.video.width) / float(frame.video.height) : 0.0f;
        videoFrame.frame_format_type = kNdiFrameFormatProgressive;
        videoFrame.timecode = kNdiTimecodeSynthesize;
        videoFrame.p_data = reinterpret_cast<quint8*>(m_videoBuffer.data());
        videoFrame.line_stride_in_bytes = frame.video.width;
        videoFrame.p_metadata = nullptr;
        videoFrame.timestamp = 0;
        m_sendVideo(m_sender, &videoFrame);

        sendAudio(frame.audio);
        return true;
    }

private:
    bool ensureLoaded() {
        if (m_loaded) return true;

        QStringList candidates;
        const QByteArray explicitPath = qgetenv("OLR_NDI_RUNTIME_LIBRARY");
        if (!explicitPath.isEmpty()) candidates.append(QString::fromLocal8Bit(explicitPath));

#if defined(Q_OS_WIN)
        const QString runtimeDir = QString::fromLocal8Bit(qgetenv("NDI_RUNTIME_DIR_V6"));
        if (!runtimeDir.isEmpty())
            candidates.append(
                QDir(runtimeDir).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll")));
        candidates.append(QStringLiteral("Processing.NDI.Lib.x64.dll"));
#elif defined(Q_OS_MACOS)
        candidates.append(QStringLiteral("/usr/local/lib/libndi.dylib"));
        candidates.append(QStringLiteral("libndi.dylib"));
#else
        candidates.append(QStringLiteral("libndi.so"));
#endif

        for (const QString& candidate : candidates) {
            if (candidate.isEmpty()) continue;
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
        m_sendCreate =
            reinterpret_cast<NDIlib_send_create_fn>(m_library.resolve("NDIlib_send_create"));
        m_sendDestroy =
            reinterpret_cast<NDIlib_send_destroy_fn>(m_library.resolve("NDIlib_send_destroy"));
        m_sendVideo = reinterpret_cast<NDIlib_send_send_video_v2_fn>(
            m_library.resolve("NDIlib_send_send_video_v2"));
        m_sendAudio = reinterpret_cast<NDIlib_send_send_audio_v3_fn>(
            m_library.resolve("NDIlib_send_send_audio_v3"));
        return m_sendCreate && m_sendDestroy && m_sendVideo;
    }

    bool packI420(const MediaVideoFrame& frame) {
        if (!frame.isValid() || (frame.width % 2) != 0 || (frame.height % 2) != 0) return false;
        const int chromaW = frame.width / 2;
        const int chromaH = frame.height / 2;
        const int yBytes = frame.width * frame.height;
        const int chromaBytes = chromaW * chromaH;
        m_videoBuffer.resize(yBytes + 2 * chromaBytes);

        char* y = m_videoBuffer.data();
        char* u = y + yBytes;
        char* v = u + chromaBytes;
        return copyPlane(frame.planeY, frame.strideY, frame.width, frame.height, y, frame.width) &&
               copyPlane(frame.planeU, frame.strideU, chromaW, chromaH, u, chromaW) &&
               copyPlane(frame.planeV, frame.strideV, chromaW, chromaH, v, chromaW);
    }

    void sendAudio(const MediaAudioFrame& audio) {
        if (!m_sendAudio || audio.format != MediaSampleFormat::S16Interleaved ||
            audio.sampleRate <= 0 || audio.channels <= 0) {
            return;
        }
        const int sampleFrames = audio.sampleFrames();
        if (sampleFrames <= 0) return;

        const int expectedBytes = sampleFrames * audio.channels * int(sizeof(qint16));
        if (audio.pcm.size() < expectedBytes) return;

        m_audioFloat.resize(sampleFrames * audio.channels);
        const auto* in = reinterpret_cast<const qint16*>(audio.pcm.constData());
        for (int sample = 0; sample < sampleFrames; ++sample) {
            for (int ch = 0; ch < audio.channels; ++ch) {
                m_audioFloat[ch * sampleFrames + sample] =
                    qBound(-1.0f, float(in[sample * audio.channels + ch]) / 32768.0f, 1.0f);
            }
        }

        NDIlib_audio_frame_v3_t audioFrame;
        audioFrame.sample_rate = audio.sampleRate;
        audioFrame.no_channels = audio.channels;
        audioFrame.no_samples = sampleFrames;
        audioFrame.timecode = kNdiTimecodeSynthesize;
        audioFrame.FourCC = kNdiFourCcFltp;
        audioFrame.p_data = reinterpret_cast<quint8*>(m_audioFloat.data());
        audioFrame.channel_stride_in_bytes = sampleFrames * int(sizeof(float));
        audioFrame.p_metadata = nullptr;
        audioFrame.timestamp = 0;
        m_sendAudio(m_sender, &audioFrame);
    }

    QLibrary m_library;
    bool m_loaded = false;
    bool m_initialized = false;
    NDIlib_send_instance_t m_sender = nullptr;
    QByteArray m_senderNameUtf8;
    FrameRate m_rate;
    QByteArray m_videoBuffer;
    QVector<float> m_audioFloat;

    NDIlib_initialize_fn m_initialize = nullptr;
    NDIlib_destroy_fn m_destroy = nullptr;
    NDIlib_send_create_fn m_sendCreate = nullptr;
    NDIlib_send_destroy_fn m_sendDestroy = nullptr;
    NDIlib_send_send_video_v2_fn m_sendVideo = nullptr;
    NDIlib_send_send_audio_v3_fn m_sendAudio = nullptr;
};

} // namespace

NdiOutputSink::NdiOutputSink() : m_ownedBackend(std::make_unique<NdiDynamicSenderBackend>()) {
    m_backend = m_ownedBackend.get();
}

NdiOutputSink::NdiOutputSink(INdiSenderBackend* backend) : m_backend(backend) {}

NdiOutputSink::~NdiOutputSink() {
    stop();
}

bool NdiOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    stop();
    m_status.framesSubmitted = 0;
    m_status.sendFailures = 0;
    m_status.lastFrameIdentity = OutputFrameIdentity();

    if (!m_backend || assignment.kind != OutputTargetKind::Ndi || !assignment.enabled ||
        !rate.isValid()) {
        setStatus(NdiOutputState::InvalidAssignment,
                  QStringLiteral("invalid NDI output assignment"));
        return false;
    }

    if (!m_backend->isRuntimeAvailable()) {
        setStatus(NdiOutputState::RuntimeUnavailable,
                  QStringLiteral("NDI runtime is not available"));
        return false;
    }

    const QString senderName = senderNameFor(assignment);
    if (senderName.isEmpty()) {
        setStatus(NdiOutputState::InvalidAssignment, QStringLiteral("NDI sender name is empty"));
        return false;
    }

    if (!m_backend->createSender(senderName, rate)) {
        setStatus(NdiOutputState::CreateFailed,
                  QStringLiteral("failed to create NDI sender '%1'").arg(senderName));
        return false;
    }
    m_assignment = assignment;
    m_rate = rate;
    m_active = true;
    setStatus(NdiOutputState::Active, QStringLiteral("NDI sender '%1' active").arg(senderName));
    return true;
}

void NdiOutputSink::stop() {
    if (m_active && m_backend) m_backend->destroySender();
    m_active = false;
    setStatus(NdiOutputState::Stopped, QStringLiteral("NDI sender stopped"));
}

bool NdiOutputSink::submit(const OutputBusFrame& frame) {
    if (!m_active || !m_backend) return false;
    m_status.lastFrameIdentity = outputFrameIdentityFor(frame);
    if (!m_backend->sendFrame(frame)) {
        m_status.sendFailures++;
        setStatus(NdiOutputState::SendFailed, QStringLiteral("failed to send NDI frame"));
        return false;
    }
    m_status.framesSubmitted++;
    setStatus(NdiOutputState::Active,
              QStringLiteral("NDI sender '%1' active").arg(senderNameFor(m_assignment)));
    return true;
}

void NdiOutputSink::setStatus(NdiOutputState state, const QString& message) {
    m_status.state = state;
    m_status.message = message;
}

QString NdiOutputSink::senderNameFor(const OutputTargetAssignment& assignment) {
    const QString configured =
        assignment.settings.value(QStringLiteral("senderName")).toString().trimmed();
    if (!configured.isEmpty()) return configured;
    if (!assignment.id.trimmed().isEmpty())
        return QStringLiteral("OpenLiveReplay %1").arg(assignment.id.trimmed());

    switch (assignment.sourceBus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("OpenLiveReplay Feed %1").arg(assignment.sourceBus.index + 1);
    case OutputBusKind::Multiview:
        return QStringLiteral("OpenLiveReplay Multiview");
    case OutputBusKind::Pgm:
        return QStringLiteral("OpenLiveReplay PGM");
    }
    return QStringLiteral("OpenLiveReplay Output");
}
