#include "playback/output/ndisink.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QLibrary>
#include <QVector>

#include "playback/output/ndiabi.h"
#include "playback/output/ndiruntimepaths.h"

#include <cstring>

namespace {

using namespace olr::ndi;

bool copyPlane(const QByteArray& src, int srcStride, int width, int height, char* dst,
               int dstStride) {
    if (srcStride <= 0 || width <= 0 || height <= 0 || dstStride < width) return false;
    if (src.size() < srcStride * height) return false;
    for (int y = 0; y < height; ++y) {
        memcpy(dst + y * dstStride, src.constData() + y * srcStride, size_t(width));
    }
    return true;
}

bool hasSendableBroadcastAudio(const MediaAudioFrame& audio) {
    if (audio.format != MediaSampleFormat::S16Interleaved || audio.sampleRate <= 0 ||
        audio.channels <= 0) {
        return false;
    }

    const int sampleFrames = audio.sampleFrames();
    if (sampleFrames <= 0) return false;

    const qsizetype expectedBytes =
        qsizetype(sampleFrames) * audio.channels * qsizetype(sizeof(qint16));
    return audio.pcm.size() >= expectedBytes;
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
        if (!m_sender || !m_sendVideo || !m_sendAudio || !frame.video.isValid()) return false;
        if (!packI420(frame.video)) return false;
        NDIlib_audio_frame_v3_t audioFrame;
        if (!packAudio(frame.audio, &audioFrame)) return false;

        NDIlib_video_frame_v2_t videoFrame;
        videoFrame.xres = frame.video.width;
        videoFrame.yres = frame.video.height;
        videoFrame.FourCC = kFourCcI420;
        videoFrame.frame_rate_N = m_rate.numerator;
        videoFrame.frame_rate_D = m_rate.denominator;
        videoFrame.picture_aspect_ratio =
            frame.video.height > 0 ? float(frame.video.width) / float(frame.video.height) : 0.0f;
        videoFrame.frame_format_type = kFrameFormatProgressive;
        videoFrame.timecode = kTimecodeSynthesize;
        videoFrame.p_data = reinterpret_cast<quint8*>(m_videoBuffer.data());
        videoFrame.line_stride_in_bytes = frame.video.width;
        videoFrame.p_metadata = nullptr;
        videoFrame.timestamp = 0;
        m_sendVideo(m_sender, &videoFrame);
        m_sendAudio(m_sender, &audioFrame);
        return true;
    }

private:
    bool ensureLoaded() {
        if (m_loaded) return true;

        for (const QString& candidate : runtimeLibraryCandidates()) {
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
        return m_sendCreate && m_sendDestroy && m_sendVideo && m_sendAudio;
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

    bool packAudio(const MediaAudioFrame& audio, NDIlib_audio_frame_v3_t* audioFrame) {
        if (!audioFrame || !hasSendableBroadcastAudio(audio)) return false;

        const int sampleFrames = audio.sampleFrames();
        const int expectedBytes = sampleFrames * audio.channels * int(sizeof(qint16));
        if (audio.pcm.size() < expectedBytes) return false;

        m_audioFloat.resize(sampleFrames * audio.channels);
        const auto* in = reinterpret_cast<const qint16*>(audio.pcm.constData());
        for (int sample = 0; sample < sampleFrames; ++sample) {
            for (int ch = 0; ch < audio.channels; ++ch) {
                m_audioFloat[ch * sampleFrames + sample] =
                    qBound(-1.0f, float(in[sample * audio.channels + ch]) / 32768.0f, 1.0f);
            }
        }

        audioFrame->sample_rate = audio.sampleRate;
        audioFrame->no_channels = audio.channels;
        audioFrame->no_samples = sampleFrames;
        audioFrame->timecode = kTimecodeSynthesize;
        audioFrame->FourCC = kFourCcFltp;
        audioFrame->p_data = reinterpret_cast<quint8*>(m_audioFloat.data());
        audioFrame->channel_stride_in_bytes = sampleFrames * int(sizeof(float));
        audioFrame->p_metadata = nullptr;
        audioFrame->timestamp = 0;
        return true;
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
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.framesSubmitted = 0;
        m_status.sendFailures = 0;
        m_status.lastSendDurationNs = 0;
        m_status.hasLastFrameIdentity = false;
        m_status.lastFrameDelivered = false;
        m_status.lastFrameIdentity = OutputFrameIdentity();
    }

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

    QElapsedTimer sendTimer;
    sendTimer.start();
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.lastFrameIdentity = outputFrameIdentityFor(frame);
        m_status.hasLastFrameIdentity = true;
        m_status.lastFrameDelivered = false;
    }
    if (!hasSendableBroadcastAudio(frame.audio)) {
        {
            QMutexLocker locker(&m_statusMutex);
            m_status.sendFailures++;
            m_status.lastSendDurationNs = sendTimer.nsecsElapsed();
            m_status.lastFrameDelivered = false;
            m_status.state = NdiOutputState::SendFailed;
            m_status.message = QStringLiteral("failed to send NDI frame: missing broadcast audio");
        }
        return false;
    }
    if (!m_backend->sendFrame(frame)) {
        {
            QMutexLocker locker(&m_statusMutex);
            m_status.sendFailures++;
            m_status.lastSendDurationNs = sendTimer.nsecsElapsed();
            m_status.lastFrameDelivered = false;
            m_status.state = NdiOutputState::SendFailed;
            m_status.message = QStringLiteral("failed to send NDI frame");
        }
        return false;
    }
    {
        QMutexLocker locker(&m_statusMutex);
        m_status.framesSubmitted++;
        m_status.lastSendDurationNs = sendTimer.nsecsElapsed();
        m_status.lastFrameDelivered = true;
        m_status.state = NdiOutputState::Active;
        m_status.message =
            QStringLiteral("NDI sender '%1' active").arg(senderNameFor(m_assignment));
    }
    return true;
}

NdiOutputStatus NdiOutputSink::status() const {
    QMutexLocker locker(&m_statusMutex);
    return m_status;
}

OutputSinkStatus NdiOutputSink::outputStatus() const {
    const NdiOutputStatus ndi = status();
    OutputSinkStatus out;
    out.acceptedFrames = ndi.framesSubmitted;
    out.failedFrames = ndi.sendFailures;
    out.lastSubmitDurationNs = ndi.lastSendDurationNs;
    out.hasLastResult = ndi.framesSubmitted > 0 || ndi.sendFailures > 0;
    out.lastResultSucceeded = ndi.state != NdiOutputState::SendFailed;
    if (ndi.hasLastFrameIdentity) {
        out.hasLastQueuedFrameIndex = true;
        out.lastQueuedFrameIndex = ndi.lastFrameIdentity.outputFrameIndex;
        out.hasLastDeliveredFrameIndex =
            ndi.lastFrameDelivered && ndi.state == NdiOutputState::Active;
        if (out.hasLastDeliveredFrameIndex) {
            out.lastDeliveredFrameIndex = ndi.lastFrameIdentity.outputFrameIndex;
        }
    }
    switch (ndi.state) {
    case NdiOutputState::Stopped:
        out.state = QStringLiteral("stopped");
        break;
    case NdiOutputState::RuntimeUnavailable:
        out.state = QStringLiteral("runtime-unavailable");
        break;
    case NdiOutputState::InvalidAssignment:
        out.state = QStringLiteral("invalid");
        break;
    case NdiOutputState::CreateFailed:
        out.state = QStringLiteral("create-failed");
        break;
    case NdiOutputState::Active:
        out.state = QStringLiteral("active");
        break;
    case NdiOutputState::SendFailed:
        out.state = QStringLiteral("send-failed");
        break;
    }
    out.message = ndi.message;
    return out;
}

void NdiOutputSink::setStatus(NdiOutputState state, const QString& message) {
    QMutexLocker locker(&m_statusMutex);
    m_status.state = state;
    m_status.message = message;
}

QString NdiOutputSink::senderNameFor(const OutputTargetAssignment& assignment) {
    QString configured =
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
