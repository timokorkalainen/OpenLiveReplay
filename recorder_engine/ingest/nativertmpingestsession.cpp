#include "nativertmpingestsession.h"

#include <QDateTime>
#include <QDebug>
#include <QSslError>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThread>

namespace {
constexpr int kConnectTimeoutMs = 5000;
constexpr int kIoTimeoutMs = 5000;
constexpr int kRtmpVersion = 3;
constexpr int kMessageSetChunkSize = 1;
constexpr int kMessageAbort = 2;
constexpr int kMessageAck = 3;
constexpr int kMessageUserControl = 4;
constexpr int kMessageWindowAckSize = 5;
constexpr int kMessagePeerBandwidth = 6;
constexpr int kMessageAudio = 8;
constexpr int kMessageVideo = 9;
constexpr int kMessageCommandAmf0 = 20;
constexpr int kAudioSampleRate = 48000;
constexpr int64_t kForwardJumpMs = 3000;
constexpr int64_t kBackwardToleranceMs = -200;
constexpr int64_t kSupportedVideoProbeMs = 5000;

bool isVideoToolboxDecodeCapabilityFailure(const QString& error) {
    return error.startsWith(QStringLiteral("VideoToolbox decompression session creation failed")) ||
           error.startsWith(QStringLiteral("VideoToolbox is unavailable"));
}

} // namespace

NativeRtmpIngestSession::NativeRtmpIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                                 std::atomic<bool>* captureRunning)
    : m_sourceIndex(sourceIndex), m_outputWidth(outputWidth), m_outputHeight(outputHeight),
      m_captureRunning(captureRunning) {
    m_monotonic.start();
}

NativeRtmpIngestSession::~NativeRtmpIngestSession() {
    requestStop();
}

bool NativeRtmpIngestSession::supportsUrl(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps")) &&
           !url.host().isEmpty() && RtmpUrlParts::fromUrl(url).isValid();
}

bool NativeRtmpIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    requestStop();
    m_url = url;
    m_callbacks = callbacks;
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_videoDecoder.reset();
    m_audioDecoder.reset();
    m_avcConfig = RtmpAvcConfig();
    m_hevcConfig = RtmpHevcConfig();
    m_aacConfig = RtmpAacConfig();
    m_videoCodec = NativeVideoCodec::Unknown;
    m_streamId = 1;
    m_firstDtsMs = -1;
    m_prevDtsMs = -1;
    m_anchorStreamTimeMs = -1;
    m_firstAudioPtsMs = -1;
    m_prevAudioPtsMs = -1;
    m_audioAnchorStreamTimeMs = -1;
    m_lastPacketAtMs = m_monotonic.elapsed();
    m_seenSupportedVideo = false;
    m_seenSupportedAudio = false;
    m_openedAtMs = -1;
    m_unsupportedReason.clear();
    m_lastFailureKind = IngestFailureKind::None;
    m_chunkParser.reset();
    m_pendingMessages.clear();

    QString error;
    if (!connectAndPlay(&error)) {
        log(error);
        requestStop();
        return false;
    }

    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(true);
    }
    log(QStringLiteral("Native RTMP connected."));
    return true;
}

void NativeRtmpIngestSession::run() {
    if (!m_socket) {
        return;
    }
    m_openedAtMs = m_monotonic.elapsed();

    const auto supportedVideoProbeExpired = [this]() {
        return !m_seenSupportedVideo && m_openedAtMs >= 0 &&
               m_monotonic.elapsed() - m_openedAtMs > kSupportedVideoProbeMs;
    };
    const auto logUnsupportedVideoProbe = [this]() {
        m_lastFailureKind = IngestFailureKind::UnsupportedProfile;
        log(QStringLiteral(
            "Native RTMP unsupported profile: no supported video within probe window."));
    };

    while (!shouldStop()) {
        if (supportedVideoProbeExpired()) {
            logUnsupportedVideoProbe();
            break;
        }

        RtmpMessage message;
        QString error;
        if (!readMessage(&message, &error)) {
            if (!shouldStop()) {
                if (!m_unsupportedReason.isEmpty()) {
                    m_lastFailureKind = IngestFailureKind::UnsupportedProfile;
                } else if (supportedVideoProbeExpired()) {
                    logUnsupportedVideoProbe();
                } else {
                    log(error.isEmpty() ? QStringLiteral("Native RTMP disconnected.") : error);
                }
            }
            break;
        }
        m_lastPacketAtMs = m_monotonic.elapsed();
        processMessage(message);
        if (!m_unsupportedReason.isEmpty() ||
            shouldFallbackToFfmpegAfterNativeFailure(m_lastFailureKind)) {
            break;
        }
        if (supportedVideoProbeExpired()) {
            logUnsupportedVideoProbe();
            break;
        }
    }

    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(false);
    }
}

void NativeRtmpIngestSession::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket.reset();
    }
}

bool NativeRtmpIngestSession::connectAndPlay(QString* error) {
    if (!supportsUrl(m_url)) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        if (error)
            *error =
                QStringLiteral("Native RTMP supports rtmp:// and rtmps:// URLs with an app path.");
        return false;
    }

    const bool useTls = m_url.scheme().toLower() == QStringLiteral("rtmps");
    if (useTls) {
        auto socket = std::make_unique<QSslSocket>();
        QSslSocket* raw = socket.get();
        if (qEnvironmentVariableIsSet("OLR_NATIVE_RTMP_ALLOW_INSECURE_TLS")) {
            raw->ignoreSslErrors();
            QObject::connect(raw, &QSslSocket::sslErrors, raw,
                             [raw](const QList<QSslError>&) { raw->ignoreSslErrors(); });
        }
        raw->connectToHostEncrypted(m_url.host(), quint16(m_url.port(443)));
        if (!raw->waitForEncrypted(kConnectTimeoutMs)) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error =
                    QStringLiteral("Native RTMPS connect failed: %1").arg(raw->errorString());
            return false;
        }
        m_socket = std::move(socket);
    } else {
        m_socket = std::make_unique<QTcpSocket>();
        m_socket->connectToHost(m_url.host(), quint16(m_url.port(1935)));
    }

    if (!m_socket->waitForConnected(kConnectTimeoutMs)) {
        m_lastFailureKind = IngestFailureKind::TransientNetwork;
        if (error)
            *error = QStringLiteral("Native RTMP connect failed: %1").arg(m_socket->errorString());
        return false;
    }

    return performHandshake(error) && sendConnectCommand(error) && sendCreateStreamCommand(error) &&
           sendPlayCommand(error);
}

bool NativeRtmpIngestSession::performHandshake(QString* error) {
    QByteArray c0c1(1537, char(0));
    c0c1[0] = char(kRtmpVersion);
    const quint32 now = quint32(QDateTime::currentSecsSinceEpoch());
    c0c1[1] = char((now >> 24) & 0xff);
    c0c1[2] = char((now >> 16) & 0xff);
    c0c1[3] = char((now >> 8) & 0xff);
    c0c1[4] = char(now & 0xff);
    for (int i = 9; i < c0c1.size(); ++i) {
        c0c1[i] = char((i * 31) & 0xff);
    }
    if (!writeFully(c0c1, error)) {
        return false;
    }

    QByteArray s0s1s2(3073, Qt::Uninitialized);
    if (!readFully(s0s1s2.data(), s0s1s2.size(), error)) {
        return false;
    }
    if (uchar(s0s1s2[0]) != kRtmpVersion) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        if (error) *error = QStringLiteral("Native RTMP server used unsupported version.");
        return false;
    }
    return writeFully(s0s1s2.mid(1, 1536), error);
}

bool NativeRtmpIngestSession::sendConnectCommand(QString* error) {
    const QByteArray payload =
        RtmpAmf0::connectCommandPayload(m_url, RtmpConnectCodecProfile::AvcAac);
    if (!sendMessage(3, kMessageCommandAmf0, 0, 0, payload, error)) {
        return false;
    }
    RtmpMessage result;
    return waitForCommandResult(1, &result, error);
}

bool NativeRtmpIngestSession::sendCreateStreamCommand(QString* error) {
    QByteArray payload;
    payload.append(RtmpAmf0::string(QStringLiteral("createStream")));
    payload.append(RtmpAmf0::number(2));
    payload.append(RtmpAmf0::nullValue());
    if (!sendMessage(3, kMessageCommandAmf0, 0, 0, payload, error)) {
        return false;
    }

    RtmpMessage result;
    if (!waitForCommandResult(2, &result, error)) {
        return false;
    }

    int offset = 0;
    QString name;
    double transactionId = 0;
    if (!RtmpAmf0::readString(result.payload, &offset, &name) ||
        !RtmpAmf0::readNumber(result.payload, &offset, &transactionId) ||
        !RtmpAmf0::skipValue(result.payload, &offset)) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        if (error) *error = QStringLiteral("Native RTMP createStream response was malformed.");
        return false;
    }
    double streamNumber = 0;
    if (!RtmpAmf0::readNumber(result.payload, &offset, &streamNumber)) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        if (error) *error = QStringLiteral("Native RTMP createStream omitted stream id.");
        return false;
    }
    m_streamId = int(streamNumber);
    return m_streamId > 0;
}

bool NativeRtmpIngestSession::sendPlayCommand(QString* error) {
    const RtmpUrlParts parts = RtmpUrlParts::fromUrl(m_url);
    QByteArray payload;
    payload.append(RtmpAmf0::string(QStringLiteral("play")));
    payload.append(RtmpAmf0::number(0));
    payload.append(RtmpAmf0::nullValue());
    payload.append(RtmpAmf0::string(parts.playPath));
    payload.append(RtmpAmf0::number(-1000));
    payload.append(RtmpAmf0::number(-1));
    payload.append(RtmpAmf0::boolean(true));
    return sendMessage(8, kMessageCommandAmf0, m_streamId, 0, payload, error);
}

bool NativeRtmpIngestSession::waitForCommandResult(double transactionId, RtmpMessage* result,
                                                   QString* error) {
    while (!shouldStop()) {
        RtmpMessage message;
        if (!readMessage(&message, error)) {
            return false;
        }
        if (message.type != kMessageCommandAmf0) {
            processMessage(message);
            continue;
        }

        int offset = 0;
        QString command;
        double seenTransactionId = 0;
        if (!RtmpAmf0::readString(message.payload, &offset, &command) ||
            !RtmpAmf0::readNumber(message.payload, &offset, &seenTransactionId)) {
            continue;
        }
        if (command == QStringLiteral("_result") && seenTransactionId == transactionId) {
            if (result) *result = std::move(message);
            return true;
        }
        if (command == QStringLiteral("_error") && seenTransactionId == transactionId) {
            if (error) *error = QStringLiteral("Native RTMP command failed.");
            return false;
        }
    }
    if (error) *error = QStringLiteral("Native RTMP command cancelled.");
    return false;
}

bool NativeRtmpIngestSession::readMessage(RtmpMessage* message, QString* error) {
    if (!message || !m_socket) {
        return false;
    }

    if (!m_pendingMessages.isEmpty()) {
        *message = m_pendingMessages.takeFirst();
        return true;
    }

    while (!shouldStop()) {
        if (m_socket->bytesAvailable() <= 0 &&
            !m_socket->waitForReadyRead(kIoTimeoutMs)) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error = QStringLiteral("Native RTMP read failed: %1").arg(m_socket->errorString());
            return false;
        }

        const QByteArray bytes = m_socket->readAll();
        if (bytes.isEmpty()) {
            continue;
        }

        QList<RtmpMessage> parsed;
        if (!m_chunkParser.push(bytes, &parsed, error)) {
            m_lastFailureKind = IngestFailureKind::MalformedStream;
            if (error && !error->startsWith(QStringLiteral("Native RTMP"))) {
                *error = QStringLiteral("Native RTMP parse failed: %1").arg(*error);
            }
            return false;
        }
        m_pendingMessages.append(parsed);
        if (!m_pendingMessages.isEmpty()) {
            *message = m_pendingMessages.takeFirst();
            return true;
        }
    }
    if (error) *error = QStringLiteral("Native RTMP read cancelled.");
    return false;
}

bool NativeRtmpIngestSession::sendMessage(int chunkStreamId, int messageType, int messageStreamId,
                                          qint64 timestampMs, const QByteArray& payload,
                                          QString* error) {
    return writeFully(RtmpChunkWriter::message(chunkStreamId, messageType, messageStreamId,
                                               timestampMs, payload, m_outputChunkSize),
                      error);
}

bool NativeRtmpIngestSession::readFully(char* data, qsizetype size, QString* error) {
    qsizetype offset = 0;
    while (offset < size && !shouldStop()) {
        const qint64 read = m_socket->read(data + offset, size - offset);
        if (read > 0) {
            offset += read;
            continue;
        }
        if (!m_socket->waitForReadyRead(kIoTimeoutMs)) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error = QStringLiteral("Native RTMP read failed: %1").arg(m_socket->errorString());
            return false;
        }
    }
    return offset == size;
}

bool NativeRtmpIngestSession::writeFully(const QByteArray& bytes, QString* error) {
    if (!m_socket) {
        return false;
    }
    qint64 written = 0;
    while (written < bytes.size() && !shouldStop()) {
        const qint64 n = m_socket->write(bytes.constData() + written, bytes.size() - written);
        if (n < 0) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error =
                    QStringLiteral("Native RTMP write failed: %1").arg(m_socket->errorString());
            return false;
        }
        written += n;
        if (!m_socket->waitForBytesWritten(kIoTimeoutMs)) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error =
                    QStringLiteral("Native RTMP write timed out: %1").arg(m_socket->errorString());
            return false;
        }
    }
    return written == bytes.size();
}

bool NativeRtmpIngestSession::shouldStop() const {
    if (m_stopRequested.load(std::memory_order_relaxed)) {
        return true;
    }
    if (m_captureRunning && !m_captureRunning->load(std::memory_order_relaxed)) {
        return true;
    }
    return m_callbacks.shouldStop ? m_callbacks.shouldStop() : false;
}

void NativeRtmpIngestSession::log(const QString& message) const {
    if (message.isEmpty()) {
        return;
    }
    if (m_callbacks.logInfo) {
        m_callbacks.logInfo(message);
    } else {
        qDebug() << "Source" << m_sourceIndex << message;
    }
}

void NativeRtmpIngestSession::processMessage(const RtmpMessage& message) {
    if (message.type == kMessageSetChunkSize) {
        return;
    }
    if (message.type == kMessageUserControl && message.payload.size() >= 6) {
        const int eventType =
            (int(uchar(message.payload[0])) << 8) | int(uchar(message.payload[1]));
        if (eventType == 6) {
            QByteArray pong;
            pong.append(char(0));
            pong.append(char(7));
            pong.append(message.payload.constData() + 2, 4);
            QString error;
            sendMessage(2, kMessageUserControl, 0, 0, pong, &error);
        }
        return;
    }
    if (message.type == kMessageVideo) {
        processVideoMessage(message.timestampMs, message.payload);
    } else if (message.type == kMessageAudio) {
        processAudioMessage(message.timestampMs, message.payload);
    } else {
        Q_UNUSED(kMessageAbort);
        Q_UNUSED(kMessageAck);
        Q_UNUSED(kMessageWindowAckSize);
        Q_UNUSED(kMessagePeerBandwidth);
    }
}

void NativeRtmpIngestSession::processVideoMessage(qint64 timestampMs, const QByteArray& payload) {
    const auto markUnsupported = [this](const QString& reason) {
        if (m_unsupportedReason.isEmpty()) {
            m_unsupportedReason = reason;
            m_lastFailureKind = IngestFailureKind::UnsupportedProfile;
            log(QStringLiteral("Native RTMP unsupported profile: %1").arg(m_unsupportedReason));
        }
    };

    RtmpVideoPacket packet;
    QString parseError;
    if (!RtmpFlv::parseVideoPacket(payload, &packet, &parseError)) {
        if (parseError.isEmpty()) {
            parseError = QStringLiteral("RTMP video packet is malformed.");
        }
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        log(QStringLiteral("Native RTMP video parse failed: %1").arg(parseError));
        if (!payload.isEmpty() && (uchar(payload[0]) & 0x80) == 0) {
            const int codecId = uchar(payload[0]) & 0x0f;
            if (codecId != 7) {
                markUnsupported(
                    QStringLiteral("unsupported RTMP video codec id %1").arg(codecId));
            }
        }
        return;
    }

    const auto codecName = [](NativeVideoCodec codec) {
        if (codec == NativeVideoCodec::H264) return QStringLiteral("avc1");
        if (codec == NativeVideoCodec::Hevc) return QStringLiteral("hvc1");
        return QStringLiteral("unknown");
    };

    if (packet.enhancedType == RtmpEnhancedVideoPacketType::Metadata) {
        return;
    }

    if (packet.enhancedType == RtmpEnhancedVideoPacketType::Multitrack) {
        log(QStringLiteral("Native RTMP ignoring unsupported multitrack video packet."));
        return;
    }

    if (packet.enhancedType == RtmpEnhancedVideoPacketType::SequenceEnd) {
        resetVideoState();
        return;
    }

    if (packet.codec != NativeVideoCodec::H264 && packet.codec != NativeVideoCodec::Hevc) {
        const QString reason = packet.fourCc.isEmpty()
                                   ? QStringLiteral("unsupported RTMP video codec")
                                   : QStringLiteral("unsupported RTMP video codec %1")
                                         .arg(packet.fourCc);
        markUnsupported(reason);
        return;
    }

    if (packet.enhancedType == RtmpEnhancedVideoPacketType::SequenceStart) {
        QString error;
        if (packet.codec == NativeVideoCodec::Hevc) {
            if (!RtmpFlv::parseHevcSequenceHeader(packet.codecPayload, &m_hevcConfig, &error)) {
                m_lastFailureKind = IngestFailureKind::MalformedStream;
                if (!error.startsWith(QStringLiteral("Native RTMP"))) {
                    error = QStringLiteral("Native %1").arg(error);
                }
                log(error);
                return;
            }
        } else {
            if (!parseAvcSequenceHeader(packet.codecPayload, &error)) {
                m_lastFailureKind = IngestFailureKind::MalformedStream;
                log(error);
                return;
            }
        }

        m_videoCodec = packet.codec;
        m_seenSupportedVideo = true;
        if (m_videoDecoder) {
            m_videoDecoder->reset();
        }
        log(QStringLiteral("Native RTMP video codec %1.").arg(codecName(packet.codec)));
        return;
    }

    if (packet.enhancedType != RtmpEnhancedVideoPacketType::CodedFrames &&
        packet.enhancedType != RtmpEnhancedVideoPacketType::CodedFramesX) {
        return;
    }

    int nalLengthSize = 0;
    H26xParameterSets parameterSets;
    if (packet.codec == NativeVideoCodec::H264) {
        if (m_videoCodec != NativeVideoCodec::H264 || m_avcConfig.parameterSets.h264Sps.isEmpty() ||
            m_avcConfig.parameterSets.h264Pps.isEmpty()) {
            return;
        }
        nalLengthSize = m_avcConfig.nalLengthSize;
        parameterSets = m_avcConfig.parameterSets;
    } else {
        if (m_videoCodec != NativeVideoCodec::Hevc ||
            m_hevcConfig.parameterSets.hevcVps.isEmpty() ||
            m_hevcConfig.parameterSets.hevcSps.isEmpty() ||
            m_hevcConfig.parameterSets.hevcPps.isEmpty()) {
            return;
        }
        nalLengthSize = m_hevcConfig.nalLengthSize;
        parameterSets = m_hevcConfig.parameterSets;
    }

    QByteArray annexB =
        RtmpFlv::lengthPrefixedPayloadToAnnexB(packet.codecPayload, nalLengthSize);
    if (annexB.isEmpty()) {
        return;
    }

    const qint64 dtsMs = timestampMs;
    const qint64 ptsMs = timestampMs + packet.compositionTimeMs;
    const int64_t sourcePtsMs = sourcePtsMsForVideo(dtsMs, ptsMs);
    if (sourcePtsMs < 0) {
        return;
    }
    if (!m_videoDecoder) {
        m_videoDecoder = std::make_unique<VideoToolboxDecoder>(m_outputWidth, m_outputHeight);
    }

    CompressedAccessUnit unit;
    unit.codec = packet.codec;
    unit.pts90k = ptsMs * 90;
    unit.dts90k = dtsMs * 90;
    unit.annexB = std::move(annexB);
    unit.parameterSets = std::move(parameterSets);

    QString error;
    const bool decoded = m_videoDecoder->decode(
        unit,
        [this, sourcePtsMs](AVFrame* frame) {
            if (!frame) return;
            if (!m_callbacks.onVideoFrame) {
                av_frame_free(&frame);
                return;
            }
            DecodedVideoFrame decodedFrame;
            decodedFrame.frame = frame;
            decodedFrame.sourcePtsMs = sourcePtsMs;
            m_callbacks.onVideoFrame(decodedFrame);
        },
        &error);
    if (!decoded && !error.isEmpty()) {
        if (isVideoToolboxDecodeCapabilityFailure(error)) {
            m_lastFailureKind = IngestFailureKind::DecodeCapability;
        }
        log(error);
    }
}

void NativeRtmpIngestSession::resetVideoState() {
    m_videoCodec = NativeVideoCodec::Unknown;
    m_avcConfig = RtmpAvcConfig();
    m_hevcConfig = RtmpHevcConfig();
    if (m_videoDecoder) {
        m_videoDecoder->reset();
    }
}

void NativeRtmpIngestSession::processAudioMessage(qint64 timestampMs, const QByteArray& payload) {
    if (!m_callbacks.onAudioChunk || payload.size() < 2) {
        return;
    }
    const int soundFormat = (uchar(payload[0]) >> 4) & 0x0f;
    if (soundFormat != 10) {
        if (m_unsupportedReason.isEmpty()) {
            m_unsupportedReason =
                QStringLiteral("unsupported RTMP audio format id %1").arg(soundFormat);
            m_lastFailureKind = IngestFailureKind::UnsupportedProfile;
            log(QStringLiteral("Native RTMP unsupported profile: %1").arg(m_unsupportedReason));
        }
        return;
    }
    const int aacPacketType = uchar(payload[1]);
    const QByteArray aacPayload = payload.mid(2);
    if (aacPacketType == 0) {
        QString error;
        if (!parseAacSequenceHeader(aacPayload, &error)) {
            m_lastFailureKind = IngestFailureKind::MalformedStream;
            log(error);
        } else {
            m_seenSupportedAudio = true;
        }
        return;
    }
    if (aacPacketType != 1 || m_aacConfig.audioObjectType <= 0 || aacPayload.isEmpty()) {
        return;
    }

    const QByteArray header = RtmpFlv::adtsHeader(m_aacConfig, aacPayload.size());
    if (header.isEmpty()) {
        return;
    }
    QByteArray frame = header;
    frame.append(aacPayload);

    AacAdtsFrameInfo info;
    if (!AudioToolboxAacDecoder::parseAdtsFrame(frame, 0, &info)) {
        return;
    }
    const int64_t sourcePtsMs = sourcePtsMsForAudio(timestampMs);
    if (sourcePtsMs < 0) {
        return;
    }
    if (!m_audioDecoder) {
        m_audioDecoder = std::make_unique<AudioToolboxAacDecoder>();
    }
    // RTMP carries raw AAC frames with a separate AudioSpecificConfig. Wrapping
    // each frame as ADTS and using a fresh converter avoids stale packet state.
    m_audioDecoder->reset();

    QByteArray pcm;
    QString error;
    if (m_audioDecoder->decodeAdtsFrame(frame, info, &pcm, &error)) {
        if (!pcm.isEmpty()) {
            DecodedAudioChunk chunk;
            chunk.startSample = sourcePtsMs * kAudioSampleRate / 1000;
            chunk.pcmS16Stereo = std::move(pcm);
            m_callbacks.onAudioChunk(std::move(chunk));
        }
    } else if (!error.isEmpty()) {
        log(error);
    }
}

bool NativeRtmpIngestSession::parseAvcSequenceHeader(const QByteArray& payload, QString* error) {
    if (!RtmpFlv::parseAvcSequenceHeader(payload, &m_avcConfig, error)) {
        if (error && !error->startsWith(QStringLiteral("Native RTMP"))) {
            *error = QStringLiteral("Native %1").arg(*error);
        }
        return false;
    }
    if (m_videoDecoder) {
        m_videoDecoder->reset();
    }
    return true;
}

bool NativeRtmpIngestSession::parseAacSequenceHeader(const QByteArray& payload, QString* error) {
    if (!RtmpFlv::parseAacSequenceHeader(payload, &m_aacConfig, error)) {
        if (error && !error->startsWith(QStringLiteral("Native RTMP"))) {
            *error = QStringLiteral("Native %1").arg(*error);
        }
        return false;
    }
    if (m_audioDecoder) {
        m_audioDecoder->reset();
    }
    return true;
}

int64_t NativeRtmpIngestSession::sourcePtsMsForVideo(qint64 dtsMs, qint64 ptsMs) {
    bool needAnchor = m_firstDtsMs < 0;
    if (!needAnchor && m_prevDtsMs >= 0) {
        const int64_t deltaMs = dtsMs - m_prevDtsMs;
        if (deltaMs > kForwardJumpMs || deltaMs < kBackwardToleranceMs) {
            log(QStringLiteral("Native RTMP DTS discontinuity (%1 ms jump). Re-anchoring.")
                    .arg(deltaMs));
            needAnchor = true;
        }
    }
    if (needAnchor) {
        m_firstDtsMs = dtsMs;
        m_anchorStreamTimeMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    }
    m_prevDtsMs = dtsMs;
    if (m_anchorStreamTimeMs < 0) {
        return -1;
    }
    return m_anchorStreamTimeMs + (ptsMs - m_firstDtsMs);
}

int64_t NativeRtmpIngestSession::sourcePtsMsForAudio(qint64 ptsMs) {
    bool needAnchor = m_firstAudioPtsMs < 0;
    if (!needAnchor && m_prevAudioPtsMs >= 0) {
        const int64_t deltaMs = ptsMs - m_prevAudioPtsMs;
        if (deltaMs > kForwardJumpMs || deltaMs < kBackwardToleranceMs) {
            log(QStringLiteral("Native RTMP audio PTS discontinuity (%1 ms jump). Re-anchoring.")
                    .arg(deltaMs));
            needAnchor = true;
            if (m_audioDecoder) {
                m_audioDecoder->reset();
            }
        }
    }
    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    if (needAnchor) {
        m_firstAudioPtsMs = ptsMs;
        m_audioAnchorStreamTimeMs = nowMs;
    }
    m_prevAudioPtsMs = ptsMs;
    if (m_audioAnchorStreamTimeMs < 0) {
        return -1;
    }
    return m_audioAnchorStreamTimeMs + (ptsMs - m_firstAudioPtsMs);
}
