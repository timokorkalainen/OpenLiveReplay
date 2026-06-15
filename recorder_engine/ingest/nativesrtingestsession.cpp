#include "nativesrtingestsession.h"

#include <QDebug>
#include <QThread>
#include <QUrlQuery>

#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <mutex>
#include <sys/socket.h>
#include <utility>

#include <srt/srt.h>

namespace {
constexpr int kSrtReceiveBufferSize = 1316;
constexpr int kSrtLatencyMs = 500;
constexpr int kSrtConnectTimeoutMs = 5000;
constexpr int kPollSleepMs = 10;
constexpr int kConnectPollSleepMs = 50;
constexpr int kStallTimeoutMs = 8000;
constexpr int64_t kForwardJump90k = 3000 * 90;
constexpr int64_t kBackwardTolerance90k = -200 * 90;
constexpr int kTsPacketSize = 188;

std::mutex srtLibraryMutex;
int srtLibraryRefs = 0;

bool isNumericIpv4Host(const QString& host) {
    sockaddr_in address {};
    return inet_pton(AF_INET, host.toUtf8().constData(), &address.sin_addr) == 1;
}

bool acquireSrtLibrary(QString* error) {
    std::lock_guard<std::mutex> lock(srtLibraryMutex);
    if (srtLibraryRefs == 0 && srt_startup() == SRT_ERROR) {
        if (error) {
            *error = QStringLiteral("Native SRT startup failed: %1")
                         .arg(QString::fromUtf8(srt_getlasterror_str()));
        }
        return false;
    }
    ++srtLibraryRefs;
    return true;
}

void releaseSrtLibrary() {
    std::lock_guard<std::mutex> lock(srtLibraryMutex);
    if (srtLibraryRefs <= 0) {
        return;
    }
    --srtLibraryRefs;
    if (srtLibraryRefs == 0) {
        srt_cleanup();
    }
}

bool setSrtOption(SRTSOCKET socket, SRT_SOCKOPT option, const void* value, int size,
                  QString* error, const QString& name) {
    if (srt_setsockopt(socket, 0, option, value, size) == SRT_ERROR) {
        if (error) {
            *error = QStringLiteral("Native SRT %1 failed: %2")
                         .arg(name, QString::fromUtf8(srt_getlasterror_str()));
        }
        return false;
    }
    return true;
}

bool isAsyncReceivePending() {
    int osError = 0;
    const int code = srt_getlasterror(&osError);
    Q_UNUSED(osError);
    return code == SRT_EASYNCRCV;
}

int findAlignedSyncOffset(const QByteArray& bytes) {
    for (int i = 0; i < bytes.size(); ++i) {
        if (bytes.at(i) != char(0x47)) {
            continue;
        }
        if (i + kTsPacketSize >= bytes.size() || bytes.at(i + kTsPacketSize) == char(0x47)) {
            return i;
        }
    }
    return -1;
}

} // namespace

NativeSrtIngestSession::NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning)
    : m_sourceIndex(sourceIndex)
    , m_outputWidth(outputWidth)
    , m_outputHeight(outputHeight)
    , m_captureRunning(captureRunning) {
    m_monotonic.start();
}

NativeSrtIngestSession::~NativeSrtIngestSession() {
    requestStop();
    closeSocket();
}

bool NativeSrtIngestSession::supportsUrl(const QUrl& url) {
    if (url.scheme().toLower() != QStringLiteral("srt")) {
        return false;
    }

    const QUrlQuery query(url);
    const QString mode = query.queryItemValue(QStringLiteral("mode")).toLower();
    if (!mode.isEmpty() && mode != QStringLiteral("caller")) {
        return false;
    }
    if (query.hasQueryItem(QStringLiteral("passphrase"))
        || query.hasQueryItem(QStringLiteral("pbkeylen"))) {
        return false;
    }

    return isNumericIpv4Host(url.host());
}

bool NativeSrtIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    closeSocket();
    m_url = url;
    m_callbacks = callbacks;
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_tsBuffer.clear();
    m_tsParser = MpegTsParser();
    m_activeCodec = NativeVideoCodec::Unknown;
    m_splitter.reset();
    m_decoder.reset();
    m_firstDts90k = -1;
    m_prevDts90k = -1;
    m_anchorStreamTimeMs = -1;
    m_lastPacketAtMs = m_monotonic.elapsed();

    QString error;
    if (!openSocket(&error)) {
        log(error);
        return false;
    }

    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(true);
    }
    log(QStringLiteral("Native SRT connected."));
    return true;
}

void NativeSrtIngestSession::run() {
    if (m_socket == SRT_INVALID_SOCK) {
        return;
    }

    QByteArray buffer(kSrtReceiveBufferSize, Qt::Uninitialized);
    while (!shouldStop()) {
        const int received = srt_recv(m_socket, buffer.data(), int(buffer.size()));
        if (received > 0) {
            m_lastPacketAtMs = m_monotonic.elapsed();
            processReceivedBytes(buffer.constData(), received);
            continue;
        }

        if (isAsyncReceivePending()) {
            if (m_lastPacketAtMs >= 0 && m_monotonic.elapsed() - m_lastPacketAtMs > kStallTimeoutMs) {
                log(QStringLiteral("Native SRT stalled. Restarting..."));
                break;
            }
            QThread::msleep(kPollSleepMs);
            continue;
        }

        const SRT_SOCKSTATUS state = srt_getsockstate(m_socket);
        if (state == SRTS_BROKEN || state == SRTS_NONEXIST || state == SRTS_CLOSED) {
            log(QStringLiteral("Native SRT disconnected."));
        } else {
            log(QStringLiteral("Native SRT receive failed: %1")
                    .arg(QString::fromUtf8(srt_getlasterror_str())));
        }
        break;
    }

    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(false);
    }
}

void NativeSrtIngestSession::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    closeSocket();
}

bool NativeSrtIngestSession::openSocket(QString* error) {
    if (!acquireSrtLibrary(error)) {
        return false;
    }
    m_srtLibraryStarted = true;

    m_socket = srt_create_socket();
    if (m_socket == SRT_INVALID_SOCK) {
        if (error) {
            *error = QStringLiteral("Native SRT socket creation failed: %1")
                         .arg(QString::fromUtf8(srt_getlasterror_str()));
        }
        closeSocket();
        return false;
    }

    const int no = 0;
    const int yes = 1;
    const int latency = kSrtLatencyMs;
    const int connectTimeout = kSrtConnectTimeoutMs;
    const SRT_TRANSTYPE transType = SRTT_LIVE;

    if (!setSrtOption(m_socket, SRTO_SNDSYN, &no, sizeof(no), error,
                      QStringLiteral("SRTO_SNDSYN"))
        || !setSrtOption(m_socket, SRTO_RCVSYN, &no, sizeof(no), error,
                         QStringLiteral("SRTO_RCVSYN"))
        || !setSrtOption(m_socket, SRTO_TRANSTYPE, &transType, sizeof(transType), error,
                         QStringLiteral("SRTO_TRANSTYPE"))
        || !setSrtOption(m_socket, SRTO_LATENCY, &latency, sizeof(latency), error,
                         QStringLiteral("SRTO_LATENCY"))
        || !setSrtOption(m_socket, SRTO_CONNTIMEO, &connectTimeout, sizeof(connectTimeout), error,
                         QStringLiteral("SRTO_CONNTIMEO"))
        || !setSrtOption(m_socket, SRTO_REUSEADDR, &yes, sizeof(yes), error,
                         QStringLiteral("SRTO_REUSEADDR"))) {
        closeSocket();
        return false;
    }

    const QByteArray host = m_url.host().toUtf8();
    if (host.isEmpty()) {
        if (error) *error = QStringLiteral("Native SRT URL is missing a host.");
        closeSocket();
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(quint16(m_url.port(9000)));
    if (inet_pton(AF_INET, host.constData(), &address.sin_addr) != 1) {
        if (error) {
            *error = QStringLiteral("Native SRT currently requires a numeric IPv4 host.");
        }
        closeSocket();
        return false;
    }

    const int connectResult =
        srt_connect(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connectResult == SRT_ERROR) {
        int osError = 0;
        const int code = srt_getlasterror(&osError);
        Q_UNUSED(osError);
        if (code != SRT_EASYNCSND) {
            if (error) {
                *error = QStringLiteral("Native SRT connect failed: %1")
                             .arg(QString::fromUtf8(srt_getlasterror_str()));
            }
            closeSocket();
            return false;
        }
    }

    QElapsedTimer connectTimer;
    connectTimer.start();
    while (!shouldStop()) {
        const SRT_SOCKSTATUS state = srt_getsockstate(m_socket);
        if (state == SRTS_CONNECTED) {
            return true;
        }
        if (state == SRTS_BROKEN || state == SRTS_NONEXIST || state == SRTS_CLOSED) {
            if (error) {
                *error = QStringLiteral("Native SRT connect failed: %1")
                             .arg(QString::fromUtf8(srt_getlasterror_str()));
            }
            closeSocket();
            return false;
        }
        if (connectTimer.elapsed() > kSrtConnectTimeoutMs) {
            if (error) *error = QStringLiteral("Native SRT connect timed out.");
            closeSocket();
            return false;
        }
        QThread::msleep(kConnectPollSleepMs);
    }

    if (error) *error = QStringLiteral("Native SRT connect cancelled.");
    closeSocket();
    return false;
}

void NativeSrtIngestSession::closeSocket() {
    if (m_socket != SRT_INVALID_SOCK) {
        srt_close(m_socket);
        m_socket = SRT_INVALID_SOCK;
    }
    if (m_srtLibraryStarted) {
        m_srtLibraryStarted = false;
        releaseSrtLibrary();
    }
}

bool NativeSrtIngestSession::shouldStop() const {
    if (m_stopRequested.load(std::memory_order_relaxed)) {
        return true;
    }
    if (m_captureRunning && !m_captureRunning->load(std::memory_order_relaxed)) {
        return true;
    }
    return m_callbacks.shouldStop ? m_callbacks.shouldStop() : false;
}

void NativeSrtIngestSession::log(const QString& message) const {
    if (message.isEmpty()) {
        return;
    }
    if (m_callbacks.logInfo) {
        m_callbacks.logInfo(message);
    } else {
        qDebug() << "Source" << m_sourceIndex << message;
    }
}

void NativeSrtIngestSession::processReceivedBytes(const char* data, int size) {
    if (!data || size <= 0) {
        return;
    }

    m_tsBuffer.append(data, size);
    while (m_tsBuffer.size() >= kTsPacketSize) {
        if (m_tsBuffer.at(0) != char(0x47)) {
            const int syncOffset = findAlignedSyncOffset(m_tsBuffer);
            if (syncOffset < 0) {
                const int bytesToDrop =
                    std::max(1, int(m_tsBuffer.size()) - (kTsPacketSize - 1));
                m_tsBuffer.remove(0, bytesToDrop);
                return;
            }
            m_tsBuffer.remove(0, syncOffset);
            if (m_tsBuffer.size() < kTsPacketSize) {
                return;
            }
        }

        const QByteArray packet = m_tsBuffer.left(kTsPacketSize);

        QList<PesPacket> completedPes;
        if (!m_tsParser.pushTsPacket(packet, &completedPes)) {
            m_tsBuffer.remove(0, 1);
            continue;
        }
        m_tsBuffer.remove(0, kTsPacketSize);
        for (const PesPacket& pes : std::as_const(completedPes)) {
            processPesPacket(pes);
        }
    }
}

void NativeSrtIngestSession::processPesPacket(const PesPacket& pes) {
    if (pes.kind != NativeElementaryStreamKind::Video
        || pes.videoCodec == NativeVideoCodec::Unknown) {
        return;
    }

    if (!m_splitter || m_activeCodec != pes.videoCodec) {
        m_activeCodec = pes.videoCodec;
        m_splitter = std::make_unique<H26xAccessUnitSplitter>(pes.videoCodec);
        m_decoder.reset();
        m_firstDts90k = -1;
        m_prevDts90k = -1;
        m_anchorStreamTimeMs = -1;
    }

    const QList<CompressedAccessUnit> units =
        m_splitter->pushPesPayload(pes.payload, pes.pts90k, pes.dts90k);
    if (units.isEmpty()) {
        return;
    }

    if (!m_decoder) {
        m_decoder = std::make_unique<VideoToolboxDecoder>(m_outputWidth, m_outputHeight);
    }

    for (const CompressedAccessUnit& unit : units) {
        const int64_t sourcePtsMs = sourcePtsMsForUnit(unit);
        if (sourcePtsMs < 0) {
            continue;
        }

        QString error;
        const bool decoded = m_decoder->decode(
            unit,
            [this, sourcePtsMs](AVFrame* frame) {
                if (!frame) {
                    return;
                }
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
            log(error);
        }
    }
}

int64_t NativeSrtIngestSession::sourcePtsMsForUnit(const CompressedAccessUnit& unit) {
    const int64_t unitDts90k = unit.dts90k >= 0 ? unit.dts90k : unit.pts90k;
    const int64_t unitPts90k = unit.pts90k >= 0 ? unit.pts90k : unitDts90k;
    if (unitDts90k < 0 || unitPts90k < 0) {
        return -1;
    }

    bool needAnchor = m_firstDts90k < 0;
    if (!needAnchor && m_prevDts90k >= 0) {
        const int64_t delta90k = unitDts90k - m_prevDts90k;
        if (delta90k > kForwardJump90k || delta90k < kBackwardTolerance90k) {
            log(QStringLiteral("Native SRT DTS discontinuity (%1 ms jump). Re-anchoring.")
                    .arg(delta90k / 90));
            needAnchor = true;
        }
    }

    if (needAnchor) {
        m_firstDts90k = unitDts90k;
        m_anchorStreamTimeMs =
            m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    }
    m_prevDts90k = unitDts90k;

    if (m_anchorStreamTimeMs < 0) {
        return -1;
    }
    return m_anchorStreamTimeMs + ((unitPts90k - m_firstDts90k) / 90);
}
