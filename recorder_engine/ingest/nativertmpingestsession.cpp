#include "nativertmpingestsession.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDebug>
#include <QSslError>
#include <QSslSocket>
#include <QStringList>
#include <QTcpSocket>
#include <QThread>

extern "C" {
#include <libavutil/frame.h>
}

namespace {
constexpr int kConnectTimeoutMs = 5000;
constexpr int kIoTimeoutMs = 5000;
constexpr int kSocketPollMs = 100;
constexpr int kStallTimeoutMs = 8000;
constexpr int kRtmpVersion = 3;
constexpr int kMessageSetChunkSize = 1;
constexpr int kMessageAbort = 2;
constexpr int kMessageAck = 3;
constexpr int kMessageUserControl = 4;
constexpr int kMessageWindowAckSize = 5;
constexpr int kMessagePeerBandwidth = 6;
constexpr int kMessageAudio = 8;
constexpr int kMessageVideo = 9;
constexpr int kMessageDataAmf3 = 15;
constexpr int kMessageDataAmf0 = 18;
constexpr int kMessageCommandAmf0 = 20;
constexpr int kAudioSampleRate = 48000;
constexpr int64_t kForwardJumpMs = 3000;
constexpr int64_t kBackwardToleranceMs = -200;
constexpr int64_t kSupportedVideoProbeMs = 5000;
constexpr int kMaxAmf0ScanDepth = 64;
constexpr char kReconnectRequestCode[] = "NetConnection.Connect.ReconnectRequest";

enum class Amf0StringScanResult {
    NotFound,
    Found,
    Malformed,
};

bool isVideoToolboxDecodeCapabilityFailure(const QString& error) {
    return error.startsWith(QStringLiteral("VideoToolbox decompression session creation failed")) ||
           error.startsWith(QStringLiteral("VideoToolbox is unavailable"));
}

bool needAmf0Bytes(const QByteArray& data, int offset, int size) {
    return offset < 0 || size < 0 || offset + size > data.size();
}

bool readAmf0StringBody(const QByteArray& data, int* offset, int sizeBytes, QString* value) {
    if (!offset || !value || needAmf0Bytes(data, *offset, sizeBytes)) {
        return false;
    }
    quint32 size = 0;
    for (int i = 0; i < sizeBytes; ++i) {
        size = (size << 8) | quint32(uchar(data[*offset + i]));
    }
    *offset += sizeBytes;
    if (size > quint32(data.size()) || needAmf0Bytes(data, *offset, int(size))) {
        return false;
    }
    *value = QString::fromUtf8(data.constData() + *offset, int(size));
    *offset += int(size);
    return true;
}

Amf0StringScanResult amf0ValueContainsString(const QByteArray& data, int* offset,
                                             const QString& needle, int depth);

Amf0StringScanResult scanAmf0ObjectEntries(const QByteArray& data, int* offset,
                                           const QString& needle, int depth) {
    if (!offset) {
        return Amf0StringScanResult::Malformed;
    }
    while (!needAmf0Bytes(data, *offset, 3)) {
        if (uchar(data[*offset]) == 0 && uchar(data[*offset + 1]) == 0 &&
            uchar(data[*offset + 2]) == 0x09) {
            *offset += 3;
            return Amf0StringScanResult::NotFound;
        }

        QString key;
        if (!readAmf0StringBody(data, offset, 2, &key)) {
            return Amf0StringScanResult::Malformed;
        }
        Q_UNUSED(key);

        const Amf0StringScanResult result =
            amf0ValueContainsString(data, offset, needle, depth + 1);
        if (result != Amf0StringScanResult::NotFound) {
            return result;
        }
    }
    return Amf0StringScanResult::Malformed;
}

Amf0StringScanResult amf0ValueContainsString(const QByteArray& data, int* offset,
                                             const QString& needle, int depth) {
    if (!offset || depth > kMaxAmf0ScanDepth || needAmf0Bytes(data, *offset, 1)) {
        return Amf0StringScanResult::Malformed;
    }
    int cursor = *offset;
    const int type = uchar(data[cursor++]);
    if (type == 0x00) {
        if (needAmf0Bytes(data, cursor, 8)) return Amf0StringScanResult::Malformed;
        cursor += 8;
        *offset = cursor;
        return Amf0StringScanResult::NotFound;
    }
    if (type == 0x01) {
        if (needAmf0Bytes(data, cursor, 1)) return Amf0StringScanResult::Malformed;
        cursor += 1;
        *offset = cursor;
        return Amf0StringScanResult::NotFound;
    }
    if (type == 0x02 || type == 0x0c || type == 0x0f) {
        QString value;
        if (!readAmf0StringBody(data, &cursor, type == 0x02 ? 2 : 4, &value)) {
            return Amf0StringScanResult::Malformed;
        }
        *offset = cursor;
        return value == needle ? Amf0StringScanResult::Found : Amf0StringScanResult::NotFound;
    }
    if (type == 0x03 || type == 0x08 || type == 0x10) {
        if (type == 0x08) {
            if (needAmf0Bytes(data, cursor, 4)) return Amf0StringScanResult::Malformed;
            cursor += 4;
        } else if (type == 0x10) {
            QString className;
            if (!readAmf0StringBody(data, &cursor, 2, &className)) {
                return Amf0StringScanResult::Malformed;
            }
            Q_UNUSED(className);
        }
        const Amf0StringScanResult result = scanAmf0ObjectEntries(data, &cursor, needle, depth);
        *offset = cursor;
        return result;
    }
    if (type == 0x0a) {
        if (needAmf0Bytes(data, cursor, 4)) return Amf0StringScanResult::Malformed;
        const quint32 count =
            (quint32(uchar(data[cursor])) << 24) | (quint32(uchar(data[cursor + 1])) << 16) |
            (quint32(uchar(data[cursor + 2])) << 8) | quint32(uchar(data[cursor + 3]));
        cursor += 4;
        for (quint32 i = 0; i < count; ++i) {
            const Amf0StringScanResult result =
                amf0ValueContainsString(data, &cursor, needle, depth + 1);
            if (result != Amf0StringScanResult::NotFound) {
                *offset = cursor;
                return result;
            }
        }
        *offset = cursor;
        return Amf0StringScanResult::NotFound;
    }
    if (type == 0x07) {
        if (needAmf0Bytes(data, cursor, 2)) return Amf0StringScanResult::Malformed;
        cursor += 2;
        *offset = cursor;
        return Amf0StringScanResult::NotFound;
    }
    if (type == 0x0b) {
        if (needAmf0Bytes(data, cursor, 10)) return Amf0StringScanResult::Malformed;
        cursor += 10;
        *offset = cursor;
        return Amf0StringScanResult::NotFound;
    }
    if (type == 0x05 || type == 0x06) {
        *offset = cursor;
        return Amf0StringScanResult::NotFound;
    }
    return Amf0StringScanResult::Malformed;
}

bool commandPayloadContainsReconnectRequest(const QByteArray& payload) {
    int offset = 0;
    QString command;
    double transactionId = 0;
    if (!RtmpAmf0::readString(payload, &offset, &command) ||
        !RtmpAmf0::readNumber(payload, &offset, &transactionId) ||
        command != QStringLiteral("onStatus")) {
        return false;
    }
    while (offset < payload.size()) {
        const int previousOffset = offset;
        const Amf0StringScanResult result = amf0ValueContainsString(
            payload, &offset, QString::fromLatin1(kReconnectRequestCode), 0);
        if (result == Amf0StringScanResult::Found) {
            return true;
        }
        if (result == Amf0StringScanResult::Malformed || offset <= previousOffset) {
            return false;
        }
    }
    return false;
}

// Walk the entries of one AMF0 object (0x03) or ECMA-array (0x08) body — *offset
// points just past the type marker (and, for an ECMA array, past the 4-byte count)
// — capturing the string value of the first entry whose key is one of `keys`.
// Returns Found with `out` set on a hit; advances *offset past the object on a
// clean walk; Malformed on any short/garbled read. Best-effort: any failure leaves
// the caller's AMF timecode untouched.
Amf0StringScanResult amf0ScanObjectForStringKey(const QByteArray& data, int* offset,
                                                const QStringList& keys, int depth, QString* out) {
    if (!offset || !out || depth > kMaxAmf0ScanDepth) {
        return Amf0StringScanResult::Malformed;
    }
    while (!needAmf0Bytes(data, *offset, 3)) {
        if (uchar(data[*offset]) == 0 && uchar(data[*offset + 1]) == 0 &&
            uchar(data[*offset + 2]) == 0x09) {
            *offset += 3;
            return Amf0StringScanResult::NotFound;
        }
        QString key;
        if (!readAmf0StringBody(data, offset, 2, &key)) {
            return Amf0StringScanResult::Malformed;
        }
        // A string value (type 0x02) for a matching key is the timecode.
        if (keys.contains(key) && !needAmf0Bytes(data, *offset, 1) &&
            uchar(data[*offset]) == 0x02) {
            int cursor = *offset + 1;
            QString value;
            if (!readAmf0StringBody(data, &cursor, 2, &value)) {
                return Amf0StringScanResult::Malformed;
            }
            *offset = cursor;
            *out = value;
            return Amf0StringScanResult::Found;
        }
        // Otherwise skip this value (recursing into nested objects/arrays via the
        // existing skipper with an unmatchable needle), then continue scanning.
        const Amf0StringScanResult skipped =
            amf0ValueContainsString(data, offset, QString(), depth + 1);
        if (skipped == Amf0StringScanResult::Malformed) {
            return Amf0StringScanResult::Malformed;
        }
    }
    return Amf0StringScanResult::Malformed;
}

// Scan a full AMF0 data-message payload (`@setDataFrame`/`onMetaData ... {props}`)
// for a "timecode"/"tc" string property. Returns true with `out` set on a hit.
bool amf0DataMessageTimecode(const QByteArray& payload, QString* out) {
    if (!out) {
        return false;
    }
    static const QStringList kKeys{QStringLiteral("timecode"), QStringLiteral("tc")};
    int offset = 0;
    // Walk top-level values; descend into the first object/ECMA-array we meet.
    while (!needAmf0Bytes(payload, offset, 1)) {
        const int type = uchar(payload[offset]);
        if (type == 0x08 || type == 0x03) {
            int cursor = offset + 1;
            if (type == 0x08) { // ECMA array: 4-byte associative count precedes entries.
                if (needAmf0Bytes(payload, cursor, 4)) {
                    return false;
                }
                cursor += 4;
            }
            if (amf0ScanObjectForStringKey(payload, &cursor, kKeys, 0, out) ==
                Amf0StringScanResult::Found) {
                return true;
            }
            // Not in this object; keep walking after it.
            offset = cursor;
            continue;
        }
        const int previousOffset = offset;
        // Skip a non-object top-level value (e.g. the leading strings).
        if (amf0ValueContainsString(payload, &offset, QString(), 0) ==
                Amf0StringScanResult::Malformed ||
            offset <= previousOffset) {
            return false;
        }
    }
    return false;
}

} // namespace

NativeRtmpIngestSession::NativeRtmpIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                                 std::atomic<bool>* captureRunning)
    : NativeRtmpIngestSession(sourceIndex, outputWidth, outputHeight, captureRunning, nullptr) {}

NativeRtmpIngestSession::NativeRtmpIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                                 std::atomic<bool>* captureRunning,
                                                 AnchoredSourceClock* sourceClock)
    : m_sourceIndex(sourceIndex), m_outputWidth(outputWidth), m_outputHeight(outputHeight),
      m_captureRunning(captureRunning), m_clock(sourceClock ? sourceClock : &m_ownedClock),
      m_externalClock(sourceClock != nullptr) {
    m_monotonic.start();
}

NativeRtmpIngestSession::~NativeRtmpIngestSession() {
    requestStop();
    closeSocket();
}

bool NativeRtmpIngestSession::supportsUrl(const QUrl& url) {
    const QString scheme = url.scheme().toLower();
    return (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps")) &&
           !url.host().isEmpty() && RtmpUrlParts::fromUrl(url).isValid();
}

bool NativeRtmpIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    requestStop();
    closeSocket();
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
    if (!m_externalClock) {
        m_clock->reset();
    }
    m_pendingVideoTimecode100ns = -1;
    m_amfTimecode100ns = -1;
    m_prevAudioPtsMs = -1;
    m_lastPacketAtMs = m_monotonic.elapsed();
    m_lastKeyframeAtMs = -1;
    m_lastStatsAtMs = -1;
    m_decodeFailures = 0;
    m_receivedChunkBytes = 0;
    m_nextAcknowledgementAt = 0;
    m_acknowledgementWindowSize = 0;
    m_seenSupportedVideo = false;
    m_seenSupportedAudio = false;
    m_reconnectRequested = false;
    m_openedAtMs = -1;
    m_unsupportedReason.clear();
    m_lastFailureKind = IngestFailureKind::None;
    m_chunkParser.reset();
    m_pendingMessages.clear();

    QString error;
    if (!connectAndPlay(&error)) {
        log(error);
        requestStop();
        closeSocket();
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
               m_monotonic.elapsed() - m_openedAtMs >= kSupportedVideoProbeMs;
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
        maybeReportStats();
        if (m_reconnectRequested) {
            break;
        }
        if (!m_unsupportedReason.isEmpty() || shouldStopNativeRtmpAfterFailure(m_lastFailureKind)) {
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
    closeSocket();
}

void NativeRtmpIngestSession::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
}

void NativeRtmpIngestSession::closeSocket() {
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
        QElapsedTimer connectTimer;
        connectTimer.start();
        while (!shouldStop() && !raw->isEncrypted() &&
               raw->state() != QAbstractSocket::UnconnectedState &&
               connectTimer.elapsed() <= kConnectTimeoutMs) {
            raw->waitForEncrypted(kSocketPollMs);
        }
        if (!raw->isEncrypted()) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error =
                    shouldStop()
                        ? QStringLiteral("Native RTMPS connect cancelled.")
                        : QStringLiteral("Native RTMPS connect failed: %1").arg(raw->errorString());
            return false;
        }
        m_socket = std::move(socket);
    } else {
        m_socket = std::make_unique<QTcpSocket>();
        m_socket->connectToHost(m_url.host(), quint16(m_url.port(1935)));
    }

    QElapsedTimer connectTimer;
    connectTimer.start();
    while (!shouldStop() && m_socket->state() != QAbstractSocket::ConnectedState &&
           m_socket->state() != QAbstractSocket::UnconnectedState &&
           connectTimer.elapsed() <= kConnectTimeoutMs) {
        m_socket->waitForConnected(kSocketPollMs);
    }
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_lastFailureKind = IngestFailureKind::TransientNetwork;
        if (error)
            *error =
                shouldStop()
                    ? QStringLiteral("Native RTMP connect cancelled.")
                    : QStringLiteral("Native RTMP connect failed: %1").arg(m_socket->errorString());
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
    const QByteArray payload = RtmpAmf0::connectCommandPayload(m_url, connectCodecProfile());
    if (!sendMessage(3, kMessageCommandAmf0, 0, 0, payload, error)) {
        return false;
    }
    RtmpMessage result;
    return waitForCommandResult(1, &result, error);
}

RtmpConnectCodecProfile NativeRtmpIngestSession::connectCodecProfile() {
    return RtmpConnectCodecProfile::EnhancedAvcHevcAac;
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
    if (m_streamId <= 0) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        if (error) *error = QStringLiteral("Native RTMP createStream returned invalid stream id.");
        return false;
    }
    return true;
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
        if (m_socket->bytesAvailable() <= 0 && !m_socket->waitForReadyRead(kSocketPollMs)) {
            if (m_socket->state() == QAbstractSocket::UnconnectedState) {
                m_lastFailureKind = IngestFailureKind::TransientNetwork;
                if (error)
                    *error =
                        QStringLiteral("Native RTMP read failed: %1").arg(m_socket->errorString());
                return false;
            }

            if (!m_seenSupportedVideo && m_openedAtMs >= 0 &&
                m_monotonic.elapsed() - m_openedAtMs >= kSupportedVideoProbeMs) {
                return false;
            }

            const int64_t ageMs = m_lastPacketAtMs >= 0 ? m_monotonic.elapsed() - m_lastPacketAtMs
                                                        : int64_t(kStallTimeoutMs);
            if (ageMs >= kStallTimeoutMs) {
                m_lastFailureKind = IngestFailureKind::TransientNetwork;
                if (error) *error = QStringLiteral("Native RTMP stalled.");
                return false;
            }
            maybeReportStats();
            continue;
        }

        const QByteArray bytes = m_socket->readAll();
        if (bytes.isEmpty()) {
            continue;
        }
        m_lastPacketAtMs = m_monotonic.elapsed();
        if (!acknowledgeIncomingBytes(bytes.size(), error)) {
            return false;
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
    const QByteArray bytes = RtmpChunkWriter::message(chunkStreamId, messageType, messageStreamId,
                                                      timestampMs, payload, m_outputChunkSize);
    if (bytes.isEmpty()) {
        if (error) *error = QStringLiteral("Native RTMP outgoing message is too large.");
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        return false;
    }
    return writeFully(bytes, error);
}

bool NativeRtmpIngestSession::readFully(char* data, qsizetype size, QString* error) {
    qsizetype offset = 0;
    QElapsedTimer idleTimer;
    idleTimer.start();
    while (offset < size && !shouldStop()) {
        const qint64 read = m_socket->read(data + offset, size - offset);
        if (read > 0) {
            offset += read;
            idleTimer.restart();
            continue;
        }
        if (!m_socket->waitForReadyRead(kSocketPollMs) && idleTimer.elapsed() >= kIoTimeoutMs) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error = QStringLiteral("Native RTMP read failed: %1").arg(m_socket->errorString());
            return false;
        }
    }
    if (shouldStop() && offset < size && error) {
        *error = QStringLiteral("Native RTMP read cancelled.");
    }
    return offset == size;
}

bool NativeRtmpIngestSession::writeFully(const QByteArray& bytes, QString* error) {
    if (!m_socket) {
        return false;
    }
    qint64 written = 0;
    QElapsedTimer idleTimer;
    idleTimer.start();
    while (written < bytes.size() && !shouldStop()) {
        const qint64 n = m_socket->write(bytes.constData() + written, bytes.size() - written);
        if (n < 0) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            if (error)
                *error =
                    QStringLiteral("Native RTMP write failed: %1").arg(m_socket->errorString());
            return false;
        }
        if (n == 0) {
            if (!m_socket->waitForBytesWritten(kSocketPollMs) &&
                idleTimer.elapsed() >= kIoTimeoutMs) {
                m_lastFailureKind = IngestFailureKind::TransientNetwork;
                if (error)
                    *error = QStringLiteral("Native RTMP write timed out: %1")
                                 .arg(m_socket->errorString());
                return false;
            }
            continue;
        }
        written += n;
        idleTimer.restart();
        while (m_socket->bytesToWrite() > 0 && !shouldStop()) {
            if (m_socket->waitForBytesWritten(kSocketPollMs)) {
                idleTimer.restart();
                continue;
            }
            if (idleTimer.elapsed() >= kIoTimeoutMs) {
                m_lastFailureKind = IngestFailureKind::TransientNetwork;
                if (error)
                    *error = QStringLiteral("Native RTMP write timed out: %1")
                                 .arg(m_socket->errorString());
                return false;
            }
        }
    }
    if (shouldStop() && written < bytes.size() && error) {
        *error = QStringLiteral("Native RTMP write cancelled.");
    }
    return written == bytes.size();
}

void NativeRtmpIngestSession::configureAcknowledgementWindow(quint32 windowSize) {
    m_acknowledgementWindowSize = windowSize;
    if (windowSize == 0) {
        m_nextAcknowledgementAt = 0;
        return;
    }
    m_nextAcknowledgementAt = quint64(windowSize);
}

bool NativeRtmpIngestSession::noteIncomingChunkBytes(qint64 byteCount,
                                                     quint32* acknowledgementSequence) {
    if (acknowledgementSequence) {
        *acknowledgementSequence = 0;
    }
    if (byteCount < 0) {
        return false;
    }
    if (byteCount > 0) {
        m_receivedChunkBytes += quint64(byteCount);
    }
    if (m_acknowledgementWindowSize == 0 || m_nextAcknowledgementAt == 0 ||
        m_receivedChunkBytes < m_nextAcknowledgementAt) {
        return false;
    }

    if (acknowledgementSequence) {
        *acknowledgementSequence = quint32(m_receivedChunkBytes & 0xffffffffu);
    }
    m_nextAcknowledgementAt = ((m_receivedChunkBytes / m_acknowledgementWindowSize) + 1) *
                              quint64(m_acknowledgementWindowSize);
    return true;
}

bool NativeRtmpIngestSession::acknowledgeIncomingBytes(qint64 byteCount, QString* error) {
    quint32 sequence = 0;
    if (!noteIncomingChunkBytes(byteCount, &sequence)) {
        return true;
    }

    QByteArray payload;
    payload.append(char((sequence >> 24) & 0xff));
    payload.append(char((sequence >> 16) & 0xff));
    payload.append(char((sequence >> 8) & 0xff));
    payload.append(char(sequence & 0xff));
    return sendMessage(2, kMessageAck, 0, 0, payload, error);
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

void NativeRtmpIngestSession::maybeReportStats() {
    if (!m_callbacks.reportStats || m_openedAtMs < 0) {
        return; // not yet running (handshake) or no consumer
    }
    const int64_t now = m_monotonic.elapsed();
    if (m_lastStatsAtMs >= 0 && now - m_lastStatsAtMs < 1000) {
        return; // ~1/sec
    }
    m_lastStatsAtMs = now;
    IngestStats stats;
    stats.kind = IngestStatsKind::Rtmp;
    stats.bytesTotal = m_receivedChunkBytes;
    stats.lastPacketAgeMs = m_lastPacketAtMs >= 0 ? now - m_lastPacketAtMs : 0;
    stats.keyframeAgeMs = m_lastKeyframeAtMs >= 0 ? now - m_lastKeyframeAtMs
                          : m_openedAtMs >= 0     ? now - m_openedAtMs
                                                  : 0;
    stats.decodeFailures = m_decodeFailures;
    stats.clockPpm = m_clock->ppm();
    stats.clockQuality = int(m_clock->quality());
    stats.clockLocked = m_clock->locked();
    stats.clockOffsetNs = m_clock->anchorOffsetNs();
    m_callbacks.reportStats(stats);
}

void NativeRtmpIngestSession::processMessage(const RtmpMessage& message) {
    if (message.type == kMessageSetChunkSize) {
        return;
    }
    if (message.type == kMessageWindowAckSize) {
        if (message.payload.size() != 4) {
            m_lastFailureKind = IngestFailureKind::MalformedStream;
            log(QStringLiteral("Native RTMP window acknowledgement size was malformed."));
            return;
        }
        const quint32 windowSize = (quint32(uchar(message.payload[0])) << 24) |
                                   (quint32(uchar(message.payload[1])) << 16) |
                                   (quint32(uchar(message.payload[2])) << 8) |
                                   quint32(uchar(message.payload[3]));
        configureAcknowledgementWindow(windowSize);
        QString error;
        if (!acknowledgeIncomingBytes(0, &error) && !error.isEmpty()) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            log(error);
        }
        return;
    }
    if (message.type == kMessagePeerBandwidth) {
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
    if (message.type == kMessageCommandAmf0) {
        if (commandPayloadContainsReconnectRequest(message.payload)) {
            m_lastFailureKind = IngestFailureKind::TransientNetwork;
            m_reconnectRequested = true;
            log(QStringLiteral("Native RTMP reconnect request received."));
        }
        return;
    }
    if (message.type == kMessageDataAmf0 || message.type == kMessageDataAmf3) {
        // onMetaData / @setDataFrame: best-effort AMF timecode fallback. An AMF3
        // data message is a 1-byte AMF3 marker followed by an AMF0 body; skip it.
        QByteArray amf0 = message.payload;
        if (message.type == kMessageDataAmf3 && !amf0.isEmpty()) {
            amf0.remove(0, 1);
        }
        QString timecode;
        if (amf0DataMessageTimecode(amf0, &timecode)) {
            applyAmfTimecodeString(timecode);
        }
        return;
    }
    if (message.type == kMessageVideo) {
        processVideoMessage(message.timestampMs, message.payload);
    } else if (message.type == kMessageAudio) {
        processAudioMessage(message.timestampMs, message.payload);
    } else {
        Q_UNUSED(kMessageAbort);
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

    const bool isKeyframe = !payload.isEmpty() && ((uchar(payload[0]) >> 4) & 0x07) == 1;

    RtmpVideoPacket packet;
    QString parseError;
    if (!RtmpFlv::parseVideoPacket(payload, &packet, &parseError)) {
        if (parseError.isEmpty()) {
            parseError = QStringLiteral("RTMP video packet is malformed.");
        }
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        log(QStringLiteral("Native RTMP video parse failed: %1").arg(parseError));
        if (payload.size() >= 5 && (uchar(payload[0]) & 0x80) == 0) {
            const int codecId = uchar(payload[0]) & 0x0f;
            if (codecId != 7) {
                markUnsupported(QStringLiteral("unsupported RTMP video codec id %1").arg(codecId));
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
        const QString reason =
            packet.fourCc.isEmpty()
                ? QStringLiteral("unsupported RTMP video codec")
                : QStringLiteral("unsupported RTMP video codec %1").arg(packet.fourCc);
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

    QByteArray annexB = RtmpFlv::lengthPrefixedPayloadToAnnexB(packet.codecPayload, nalLengthSize);
    if (annexB.isEmpty()) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        log(QStringLiteral(
            "Native RTMP video parse failed: malformed length-prefixed video payload."));
        return;
    }

    // Extract this access unit's SMPTE 12M timecode (SEI), falling back to the AMF
    // onMetaData timecode. Reset-then-set per unit so a frame with no TC reports
    // none (or the AMF fallback), never a previous AU's SEI TC.
    updatePendingVideoTimecode(annexB, packet.codec);

    const qint64 dtsMs = timestampMs;
    const qint64 ptsMs = timestampMs + packet.compositionTimeMs;
    const int64_t sourcePtsMs = sourcePtsMsForVideo(dtsMs, ptsMs);
    if (sourcePtsMs < 0) {
        return;
    }
    if (!m_videoDecoder) {
        m_videoDecoder = std::make_unique<NativeVideoDecoder>(m_outputWidth, m_outputHeight);
    }

    CompressedAccessUnit unit;
    unit.codec = packet.codec;
    unit.pts90k = ptsMs * 90;
    unit.dts90k = dtsMs * 90;
    unit.annexB = std::move(annexB);
    unit.parameterSets = std::move(parameterSets);

    if (isKeyframe) {
        m_lastKeyframeAtMs = m_monotonic.elapsed();
    }

    // Capture the timecode into a local (not the member) so an async decode binds
    // THIS access unit's TC even if m_pendingVideoTimecode100ns is overwritten by a
    // later AU before the callback fires.
    const int64_t timecode100ns = m_pendingVideoTimecode100ns;
    QString error;
    const bool decoded = m_videoDecoder->decode(
        unit,
        [this, sourcePtsMs, timecode100ns](AVFrame* frame) {
            if (!frame) return;
            if (!m_callbacks.onVideoFrame) {
                av_frame_free(&frame);
                return;
            }
            DecodedVideoFrame decodedFrame;
            decodedFrame.frame = frame;
            decodedFrame.sourcePtsMs = sourcePtsMs;
            decodedFrame.sourceTimecode100ns = timecode100ns;
            m_callbacks.onVideoFrame(decodedFrame);
        },
        &error);
    if (!decoded && !error.isEmpty()) {
        ++m_decodeFailures;
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
    if (!m_callbacks.onAudioChunk) {
        return;
    }
    if (payload.size() < 2) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        log(QStringLiteral("Native RTMP audio parse failed: malformed audio payload."));
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
    if (aacPacketType != 1) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        log(QStringLiteral("Native RTMP audio parse failed: malformed AAC packet type %1.")
                .arg(aacPacketType));
        return;
    }
    if (m_aacConfig.audioObjectType <= 0) {
        return;
    }
    if (aacPayload.isEmpty()) {
        m_lastFailureKind = IngestFailureKind::MalformedStream;
        log(QStringLiteral("Native RTMP audio parse failed: empty AAC payload."));
        return;
    }

    const QByteArray header = RtmpFlv::adtsHeader(m_aacConfig, static_cast<int>(aacPayload.size()));
    if (header.isEmpty()) {
        return;
    }
    QByteArray frame = header;
    frame.append(aacPayload);

    AacAdtsFrameInfo info;
    if (!NativeAacDecoder::parseAdtsFrame(frame, 0, &info)) {
        return;
    }
    const int64_t sourcePtsMs = sourcePtsMsForAudio(timestampMs);
    if (sourcePtsMs < 0) {
        return;
    }
    if (!m_audioDecoder) {
        m_audioDecoder = std::make_unique<NativeAacDecoder>();
    }

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

void NativeRtmpIngestSession::updatePendingVideoTimecode(const QByteArray& annexB,
                                                         NativeVideoCodec codec) {
    // Start from the AMF onMetaData fallback (-1 when absent): a frame with no SEI
    // timecode reports the sticky AMF TC (or none), never a previous AU's SEI TC.
    // Extraction is best-effort and bounds-checked — a garbled/truncated SEI returns
    // {valid=false}, so a bad timecode never disturbs recording.
    m_pendingVideoTimecode100ns = m_amfTimecode100ns;
    const Smpte12mTimecode tc = extractH26xSeiTimecode(annexB, codec);
    if (tc.valid) {
        m_pendingVideoTimecode100ns = Smpte12m::to100ns(tc, kTimecodeNominalFps);
    }
}

void NativeRtmpIngestSession::applyAmfTimecodeString(const QString& text) {
    // Strictly best-effort. We are only called when onMetaData actually carried a
    // timecode/tc string, so clearing on an unparseable value (rather than keeping a
    // stale one) honours the producer's latest, malformed-but-present, statement.
    const Smpte12mTimecode tc = Smpte12m::parseTimecodeString(text.toUtf8().constData());
    m_amfTimecode100ns = tc.valid ? Smpte12m::to100ns(tc, kTimecodeNominalFps) : -1;
}

int64_t NativeRtmpIngestSession::sourcePtsMsForVideo(qint64 dtsMs, qint64 ptsMs) {
    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    m_clock->observe(dtsMs, nowMs, false, ClockObservationRole::Authority);
    return m_clock->toSessionMs(ptsMs);
}

int64_t NativeRtmpIngestSession::sourcePtsMsForAudio(qint64 ptsMs) {
    if (m_clock->locked() && m_prevAudioPtsMs >= 0) {
        const int64_t deltaMs = ptsMs - m_prevAudioPtsMs;
        if (deltaMs > kForwardJumpMs || deltaMs < kBackwardToleranceMs) {
            // Audio discontinuity: flush the decoder but DO NOT move the shared anchor
            // (video owns re-anchoring) — keeps A/V locked. This is the AUD-4 model.
            log(QStringLiteral(
                    "Native RTMP audio PTS discontinuity (%1 ms jump). Flushing decoder.")
                    .arg(deltaMs));
            if (m_audioDecoder) {
                m_audioDecoder->reset();
            }
        }
    }
    m_prevAudioPtsMs = ptsMs;
    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    m_clock->observe(ptsMs, nowMs, false, ClockObservationRole::Follower);
    return m_clock->toSessionMs(ptsMs);
}
