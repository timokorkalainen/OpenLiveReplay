#include "nativesrtingestsession.h"

#include "nativesrtaddress.h"
#include "nativesrtconnectdiagnostics.h"
#include "nativesrturloptions.h"

#include <QDebug>
#include <QThread>
#include <QUrlQuery>

#include <algorithm>
#include <mutex>
#include <utility>

#include <srt/srt.h>

extern "C" {
#include <libavutil/frame.h>
}

namespace {
constexpr int kSrtReceiveBufferSize = 1316;
constexpr int kPollSleepMs = 10;
constexpr int kConnectPollSleepMs = 50;
constexpr int kStallTimeoutMs = 8000;
constexpr int64_t kForwardJump90k = 3000 * 90;
constexpr int64_t kBackwardTolerance90k = -200 * 90;
constexpr int64_t kMpegTs33Wrap90k = 1LL << 33;
constexpr int64_t kMpegTs33HalfWrap90k = 1LL << 32;
constexpr int kTsPacketSize = 188;
constexpr int kAudioSampleRate = 48000;
constexpr int kMaxAdtsFrameSize = 8191;
constexpr int64_t kAudioRemainderPtsTolerance90k = 500 * 90;

std::mutex srtLibraryMutex;
int srtLibraryRefs = 0;

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

bool setSrtOption(SRTSOCKET socket, SRT_SOCKOPT option, const void* value, int size, QString* error,
                  const QString& name) {
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

QString srtConnectFailureDetails(SRTSOCKET socket) {
    int osError = 0;
    const int code = srt_getlasterror(&osError);
    Q_UNUSED(osError);
    const QString lastError = QString::fromUtf8(srt_getlasterror_str());
    const int rejectReason = srt_getrejectreason(socket);
    const char* rejectReasonText =
        rejectReason == SRT_REJ_UNKNOWN ? nullptr : srt_rejectreason_str(rejectReason);
    return nativeSrtConnectFailureMessage(code, lastError, rejectReason,
                                          rejectReasonText ? QString::fromUtf8(rejectReasonText)
                                                           : QString());
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

qint64 samplesTo90k(qint64 samples, int sampleRate) {
    if (samples <= 0 || sampleRate <= 0) {
        return 0;
    }
    return (samples * 90000 + sampleRate / 2) / sampleRate;
}

int64_t unwrap33Bit90k(int64_t raw90k, int64_t* previousRaw90k, int64_t* wrapOffset90k) {
    if (raw90k < 0 || !previousRaw90k || !wrapOffset90k) {
        return raw90k;
    }
    if (*previousRaw90k >= 0) {
        const int64_t delta = raw90k - *previousRaw90k;
        if (delta < -kMpegTs33HalfWrap90k) {
            *wrapOffset90k += kMpegTs33Wrap90k;
        } else if (delta > kMpegTs33HalfWrap90k) {
            *wrapOffset90k -= kMpegTs33Wrap90k;
        }
    }
    *previousRaw90k = raw90k;
    return raw90k + *wrapOffset90k;
}

} // namespace

NativeSrtIngestSession::NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning)
    : NativeSrtIngestSession(sourceIndex, outputWidth, outputHeight, captureRunning, nullptr) {}

NativeSrtIngestSession::NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning,
                                               AnchoredSourceClock* sourceClock)
    : m_sourceIndex(sourceIndex), m_outputWidth(outputWidth), m_outputHeight(outputHeight),
      m_captureRunning(captureRunning), m_clock(sourceClock ? sourceClock : &m_ownedClock),
      m_externalClock(sourceClock != nullptr) {
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
    const NativeSrtUrlOptions options = nativeSrtUrlOptionsFromUrl(url);

    // Connection mode: caller (default), listener, or rendezvous are all supported
    // natively. An unrecognised mode= is genuinely-bad input and is rejected so the
    // scheme-aware unsupported diagnostic stays accurate.
    if (options.unknownMode) {
        return false;
    }

    // Encrypted SRT is supported natively (SRTO_PASSPHRASE/SRTO_PBKEYLEN). Accept a
    // valid passphrase (+ optional pbkeylen) but reject genuinely-bad encryption
    // inputs so the scheme-aware unsupported diagnostic stays accurate:
    //   - pbkeylen present without a passphrase is meaningless;
    //   - a passphrase outside SRT's 10..79-byte range (libsrt would silently leave
    //     the link UNENCRYPTED) is rejected rather than silently downgraded;
    //   - pbkeylen must be 16/24/32.
    const bool hasPassphrase = !options.passphrase.isEmpty();
    if (query.hasQueryItem(QStringLiteral("pbkeylen")) && !hasPassphrase) {
        return false;
    }
    if (hasPassphrase) {
        if (!nativeSrtPassphraseIsValid(options.passphrase) ||
            !nativeSrtPbKeyLenIsValid(options.pbKeyLen)) {
            return false;
        }
    }

    return !url.host().isEmpty();
}

NativeSrtMode NativeSrtIngestSession::connectionModeForUrl(const QUrl& url) {
    return nativeSrtUrlOptionsFromUrl(url).mode;
}

QByteArray NativeSrtIngestSession::streamIdForSocketOption(const QUrl& url) {
    return nativeSrtUrlOptionsFromUrl(url).streamId.toUtf8();
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
    m_audioDecoder.reset();
    m_audioRemainder.clear();
    if (!m_externalClock) {
        m_clock->reset();
    }
    m_prevDts90k = -1;
    m_prevAudioPts90k = -1;
    m_prevRawPcr90k = -1;
    m_prevRawVideoDts90k = -1;
    m_prevRawAudioPts90k = -1;
    m_pcrWrapOffset90k = 0;
    m_videoWrapOffset90k = 0;
    m_audioWrapOffset90k = 0;
    m_forceNextPcrObserve = true;
    m_audioRemainderPts90k = -1;
    m_lastPacketAtMs = m_monotonic.elapsed();
    m_lastDecodeErrorLogMs = -1;
    m_statRetrans = -1;
    m_statLossTotal = -1;
    m_statDropTotal = -1;
    m_statRecvTotal = -1;
    m_lastStatsAtMs = -1;
    m_loggedLatmUnsupported = false;

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
            // Snapshot SRT receiver stats ~1x/s while receiving. The socket is
            // closed only after this loop returns (the session is torn down on
            // this same capture thread), so the last in-loop snapshot is what we
            // log on exit. clear=0 keeps the counters cumulative since connect.
            if (m_lastStatsAtMs < 0 || m_monotonic.elapsed() - m_lastStatsAtMs > 1000) {
                SRT_TRACEBSTATS perf;
                if (srt_bstats(m_socket, &perf, 0) == 0) {
                    m_statRetrans = perf.pktRcvRetrans;
                    m_statLossTotal = perf.pktRcvLossTotal;
                    m_statDropTotal = perf.pktRcvDropTotal;
                    m_statRecvTotal = perf.pktRecvTotal;
                    if (m_callbacks.reportStats) {
                        IngestStats stats;
                        stats.kind = IngestStatsKind::Srt;
                        stats.recvTotal = perf.pktRecvTotal;
                        stats.retransTotal = perf.pktRcvRetrans;
                        stats.lossTotal = perf.pktRcvLossTotal;
                        stats.dropTotal = perf.pktRcvDropTotal;
                        stats.clockPpm = m_clock->ppm();
                        stats.clockQuality = int(m_clock->quality());
                        m_callbacks.reportStats(stats);
                    }
                }
                m_lastStatsAtMs = m_monotonic.elapsed();
            }
            continue;
        }

        if (isAsyncReceivePending()) {
            if (m_lastPacketAtMs >= 0 &&
                m_monotonic.elapsed() - m_lastPacketAtMs > kStallTimeoutMs) {
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

    // Loss/recovery telemetry. pktRcvRetrans>0 means SRT's ARQ retransmitted;
    // pktRcvLossTotal counts DETECTED loss (recoverable); pktRcvDropTotal is the
    // too-late-to-play loss SRT finally gave up on (== the UNRECOVERED loss).
    log(QStringLiteral(
            "srt_stats pktRcvRetrans=%1 pktRcvLossTotal=%2 pktRcvDropTotal=%3 pktRecvTotal=%4")
            .arg(m_statRetrans)
            .arg(m_statLossTotal)
            .arg(m_statDropTotal)
            .arg(m_statRecvTotal));

    if (m_callbacks.setConnected) {
        m_callbacks.setConnected(false);
    }
}

void NativeSrtIngestSession::requestStop() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    closeSocket();
}

bool NativeSrtIngestSession::openSocket(QString* error) {
    const QList<NativeSrtSockaddr> addresses = nativeSrtResolveSockaddrsWithTimeout(
        m_url.host(), quint16(m_url.port(9000)), kSrtConnectTimeoutMs,
        [this]() { return shouldStop(); });
    if (addresses.isEmpty()) {
        if (error) {
            *error =
                shouldStop()
                    ? QStringLiteral("Native SRT host lookup cancelled.")
                    : QStringLiteral("Native SRT host lookup failed for %1.").arg(m_url.host());
        }
        return false;
    }

    QString connectError;
    if (nativeSrtTrySockaddrs(
            addresses,
            [this](const NativeSrtSockaddr& address, QString* attemptError) {
                closeSocket();
                return openSocketToAddress(address, attemptError);
            },
            &connectError, [this]() { return shouldStop(); })) {
        return true;
    }

    if (error) {
        *error = connectError;
    }
    closeSocket();
    return false;
}

bool NativeSrtIngestSession::openSocketToAddress(const NativeSrtSockaddr& address, QString* error) {
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
    linger closeLinger{};
    closeLinger.l_onoff = 1;
    closeLinger.l_linger = 0;
    const SRT_TRANSTYPE transType = SRTT_LIVE;

    if (!setSrtOption(m_socket, SRTO_SNDSYN, &no, sizeof(no), error,
                      QStringLiteral("SRTO_SNDSYN")) ||
        !setSrtOption(m_socket, SRTO_RCVSYN, &no, sizeof(no), error,
                      QStringLiteral("SRTO_RCVSYN")) ||
        !setSrtOption(m_socket, SRTO_TRANSTYPE, &transType, sizeof(transType), error,
                      QStringLiteral("SRTO_TRANSTYPE")) ||
        !setSrtOption(m_socket, SRTO_LATENCY, &latency, sizeof(latency), error,
                      QStringLiteral("SRTO_LATENCY")) ||
        !setSrtOption(m_socket, SRTO_CONNTIMEO, &connectTimeout, sizeof(connectTimeout), error,
                      QStringLiteral("SRTO_CONNTIMEO")) ||
        !setSrtOption(m_socket, SRTO_REUSEADDR, &yes, sizeof(yes), error,
                      QStringLiteral("SRTO_REUSEADDR")) ||
        !setSrtOption(m_socket, SRTO_LINGER, &closeLinger, sizeof(closeLinger), error,
                      QStringLiteral("SRTO_LINGER"))) {
        closeSocket();
        return false;
    }

    const QByteArray streamId = streamIdForSocketOption(m_url);
    if (!streamId.isEmpty() &&
        !setSrtOption(m_socket, SRTO_STREAMID, streamId.constData(), streamId.size(), error,
                      QStringLiteral("SRTO_STREAMID"))) {
        closeSocket();
        return false;
    }

    // Encrypted SRT: set the key length first, then the passphrase (which is what
    // actually enables AES). supportsUrl already validated both, so by here a
    // non-empty passphrase has a 10..79-byte length and a 16/24/32 pbKeyLen.
    const NativeSrtUrlOptions urlOptions = nativeSrtUrlOptionsFromUrl(m_url);
    if (!urlOptions.passphrase.isEmpty()) {
        if (!setSrtOption(m_socket, SRTO_PBKEYLEN, &urlOptions.pbKeyLen,
                          sizeof(urlOptions.pbKeyLen), error, QStringLiteral("SRTO_PBKEYLEN"))) {
            closeSocket();
            return false;
        }
        const QByteArray passphrase = urlOptions.passphrase.toUtf8();
        if (!setSrtOption(m_socket, SRTO_PASSPHRASE, passphrase.constData(), passphrase.size(),
                          error, QStringLiteral("SRTO_PASSPHRASE"))) {
            closeSocket();
            return false;
        }
    }

    // Listener mode binds + listens + accepts the inbound caller (the engine is the
    // server). Caller (default) and rendezvous both connect out; rendezvous also
    // sets SRTO_RENDEZVOUS so the two peers connect to each other simultaneously.
    if (urlOptions.mode == NativeSrtMode::Listener) {
        return acceptListenerConnection(address, error);
    }

    if (urlOptions.mode == NativeSrtMode::Rendezvous) {
        const int yesRdv = 1;
        if (!setSrtOption(m_socket, SRTO_RENDEZVOUS, &yesRdv, sizeof(yesRdv), error,
                          QStringLiteral("SRTO_RENDEZVOUS"))) {
            closeSocket();
            return false;
        }
        // Rendezvous requires the socket to be bound to the same local port it
        // connects to (both peers are symmetric). Bind to the wildcard at that port.
        NativeSrtSockaddr bindAddress;
        if (!nativeSrtMakeIpv4Sockaddr(QStringLiteral("0.0.0.0"), quint16(m_url.port(9000)),
                                       &bindAddress) ||
            srt_bind(m_socket, bindAddress.sockaddrPtr(), bindAddress.size) == SRT_ERROR) {
            if (error) {
                *error = QStringLiteral("Native SRT rendezvous bind failed: %1")
                             .arg(QString::fromUtf8(srt_getlasterror_str()));
            }
            closeSocket();
            return false;
        }
    }

    return connectAndAwait(address, error);
}

bool NativeSrtIngestSession::connectAndAwait(const NativeSrtSockaddr& address, QString* error) {
    const int connectResult = srt_connect(m_socket, address.sockaddrPtr(), address.size);
    if (connectResult == SRT_ERROR) {
        int osError = 0;
        const int code = srt_getlasterror(&osError);
        Q_UNUSED(osError);
        if (code != SRT_EASYNCSND) {
            if (error) {
                *error = QStringLiteral("Native SRT connect failed: %1")
                             .arg(srtConnectFailureDetails(m_socket));
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
                             .arg(srtConnectFailureDetails(m_socket));
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

bool NativeSrtIngestSession::acceptListenerConnection(const NativeSrtSockaddr& address,
                                                      QString* error) {
    // Promote the configured socket to the listening socket (it already carries the
    // common options + streamid/encryption, which SRT inherits onto the accepted
    // data socket). m_socket becomes the accepted socket below.
    m_listenSocket = m_socket;
    m_socket = SRT_INVALID_SOCK;

    if (srt_bind(m_listenSocket, address.sockaddrPtr(), address.size) == SRT_ERROR) {
        if (error) {
            *error = QStringLiteral("Native SRT listener bind failed: %1")
                         .arg(QString::fromUtf8(srt_getlasterror_str()));
        }
        closeSocket();
        return false;
    }
    if (srt_listen(m_listenSocket, 1) == SRT_ERROR) {
        if (error) {
            *error = QStringLiteral("Native SRT listen failed: %1")
                         .arg(QString::fromUtf8(srt_getlasterror_str()));
        }
        closeSocket();
        return false;
    }

    // RCVSYN=0 on the listening socket makes srt_accept non-blocking, so the poll
    // loop can honor shouldStop()/timeout instead of wedging on a caller that never
    // arrives.
    QElapsedTimer acceptTimer;
    acceptTimer.start();
    while (!shouldStop()) {
        const SRTSOCKET accepted = srt_accept(m_listenSocket, nullptr, nullptr);
        if (accepted != SRT_INVALID_SOCK) {
            m_socket = accepted;
            // The data socket should poll receives non-blocking like the caller path.
            const int no = 0;
            if (!setSrtOption(m_socket, SRTO_RCVSYN, &no, sizeof(no), error,
                              QStringLiteral("SRTO_RCVSYN"))) {
                closeSocket();
                return false;
            }
            return true;
        }
        int osError = 0;
        const int code = srt_getlasterror(&osError);
        Q_UNUSED(osError);
        if (code != SRT_EASYNCRCV) {
            if (error) {
                *error = QStringLiteral("Native SRT accept failed: %1")
                             .arg(QString::fromUtf8(srt_getlasterror_str()));
            }
            closeSocket();
            return false;
        }
        if (acceptTimer.elapsed() > kSrtConnectTimeoutMs) {
            if (error) *error = QStringLiteral("Native SRT listener timed out waiting for caller.");
            closeSocket();
            return false;
        }
        QThread::msleep(kConnectPollSleepMs);
    }

    if (error) *error = QStringLiteral("Native SRT listener cancelled.");
    closeSocket();
    return false;
}

void NativeSrtIngestSession::closeSocket() {
    if (m_socket != SRT_INVALID_SOCK) {
        srt_close(m_socket);
        m_socket = SRT_INVALID_SOCK;
    }
    // Listener mode also holds a listening socket; close it so the bound port is
    // released for the next attempt (reconnect re-binds), same as the data socket.
    if (m_listenSocket != SRT_INVALID_SOCK) {
        srt_close(m_listenSocket);
        m_listenSocket = SRT_INVALID_SOCK;
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
                const int bytesToDrop = std::max(1, int(m_tsBuffer.size()) - (kTsPacketSize - 1));
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
        MpegTsParser::TsPacketInfo tsInfo;
        if (!m_tsParser.pushTsPacket(packet, &completedPes, &tsInfo)) {
            m_tsBuffer.remove(0, 1);
            continue;
        }
        m_tsBuffer.remove(0, kTsPacketSize);
        // PCR is the canonical shared anchor. A program discontinuity forces a
        // re-anchor; the first PCR (or, as a fallback, the first PES below) sets it.
        // Also clear the per-stream jump trackers so the next unit's jump heuristic
        // doesn't immediately discard the PCR re-anchor (keeps "PCR wins").
        if (tsInfo.discontinuity) {
            m_clock->reset();
            m_prevDts90k = -1;
            m_prevAudioPts90k = -1;
            m_prevRawPcr90k = -1;
            m_prevRawVideoDts90k = -1;
            m_prevRawAudioPts90k = -1;
            m_pcrWrapOffset90k = 0;
            m_videoWrapOffset90k = 0;
            m_audioWrapOffset90k = 0;
        }
        if (tsInfo.pcr90k >= 0) {
            const int64_t pcr90k = unwrapPcr90k(tsInfo.pcr90k);
            const int64_t nowMs =
                m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
            if (m_clock->locked() && !tsInfo.discontinuity && !m_forceNextPcrObserve) {
                m_clock->addRateSample(pcr90k, nowMs);
            } else {
                m_clock->observe(pcr90k, nowMs, tsInfo.discontinuity,
                                 ClockObservationRole::Authority);
                m_forceNextPcrObserve = false;
            }
        }
        for (const PesPacket& pes : std::as_const(completedPes)) {
            processPesPacket(pes);
        }
    }
}

void NativeSrtIngestSession::processPesPacket(const PesPacket& pes) {
    if (pes.kind == NativeElementaryStreamKind::AudioAacLatm) {
        if (!m_loggedLatmUnsupported) {
            log(QStringLiteral("Native SRT LATM/LOAS AAC is unsupported; continuing video."));
            m_loggedLatmUnsupported = true;
        }
        m_audioRemainder.clear();
        m_audioRemainderPts90k = -1;
        return;
    }

    if (pes.kind == NativeElementaryStreamKind::AudioAac) {
        processAudioPesPacket(pes);
        return;
    }

    if (pes.kind != NativeElementaryStreamKind::Video ||
        pes.videoCodec == NativeVideoCodec::Unknown) {
        return;
    }

    if (!m_splitter || m_activeCodec != pes.videoCodec) {
        m_activeCodec = pes.videoCodec;
        m_splitter = std::make_unique<H26xAccessUnitSplitter>(pes.videoCodec);
        m_decoder.reset();
        m_prevDts90k = -1;
    }

    const QList<CompressedAccessUnit> units =
        m_splitter->pushPesPayload(pes.payload, pes.pts90k, pes.dts90k);
    if (units.isEmpty()) {
        return;
    }

    if (!m_decoder) {
        m_decoder = std::make_unique<NativeVideoDecoder>(m_outputWidth, m_outputHeight);
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
                const int64_t decodedPtsMs = m_clock->toSessionMs(frame->pts);
                decodedFrame.sourcePtsMs = decodedPtsMs >= 0 ? decodedPtsMs : sourcePtsMs;
                m_callbacks.onVideoFrame(decodedFrame);
            },
            &error);
        if (!decoded && !error.isEmpty()) {
            const int64_t nowMs = m_monotonic.elapsed();
            if (m_lastDecodeErrorLogMs < 0 || nowMs - m_lastDecodeErrorLogMs >= 5000) {
                log(error);
                m_lastDecodeErrorLogMs = nowMs;
            }
        }
    }
}

void NativeSrtIngestSession::processAudioPesPacket(const PesPacket& pes) {
    if (!m_callbacks.onAudioChunk || pes.pts90k < 0 || pes.payload.isEmpty()) {
        return;
    }

    if (!m_audioDecoder) {
        m_audioDecoder = std::make_unique<NativeAacDecoder>();
    }

    if (!m_audioRemainder.isEmpty()) {
        const bool currentPesStartsFrame = NativeAacDecoder::hasAdtsSync(pes.payload, 0) ||
                                           NativeAacDecoder::hasLatmLoasSync(pes.payload, 0);
        const qint64 delta90k = pes.pts90k - m_audioRemainderPts90k;
        if (currentPesStartsFrame || m_audioRemainderPts90k < 0 ||
            delta90k > kAudioRemainderPtsTolerance90k ||
            delta90k < -kAudioRemainderPtsTolerance90k) {
            m_audioRemainder.clear();
            m_audioRemainderPts90k = -1;
        }
    }

    const int remainderSize = m_audioRemainder.size();
    qint64 basePts90k = pes.pts90k;
    if (remainderSize > 0 && m_audioRemainderPts90k >= 0) {
        basePts90k = m_audioRemainderPts90k;
    } else {
        m_audioRemainder.clear();
    }

    QByteArray bytes = m_audioRemainder;
    bytes.append(pes.payload);
    m_audioRemainder.clear();
    m_audioRemainderPts90k = -1;

    int offset = 0;
    qint64 consumedSamples = 0;
    int consumedSampleRate = 0;
    while (offset < bytes.size()) {
        AacAdtsFrameInfo info;
        if (!NativeAacDecoder::parseAdtsFrame(bytes, offset, &info)) {
            if (NativeAacDecoder::hasLatmLoasSync(bytes, offset)) {
                if (!m_loggedLatmUnsupported) {
                    log(QStringLiteral(
                        "Native SRT LATM/LOAS AAC is unsupported; continuing video."));
                    m_loggedLatmUnsupported = true;
                }
                m_audioRemainder.clear();
                return;
            }

            int nextOffset = -1;
            for (int i = offset + 1; i < bytes.size(); ++i) {
                if (NativeAacDecoder::parseAdtsFrame(bytes, i, nullptr) ||
                    NativeAacDecoder::hasLatmLoasSync(bytes, i)) {
                    nextOffset = i;
                    break;
                }
            }

            if (nextOffset < 0) {
                m_audioRemainder = bytes.mid(offset);
                if (m_audioRemainder.size() > kMaxAdtsFrameSize) {
                    m_audioRemainder = m_audioRemainder.right(kMaxAdtsFrameSize);
                    basePts90k = pes.pts90k;
                    consumedSamples = 0;
                    consumedSampleRate = 0;
                }
                m_audioRemainderPts90k =
                    basePts90k + samplesTo90k(consumedSamples, consumedSampleRate);
                return;
            }
            if (nextOffset >= remainderSize) {
                basePts90k = pes.pts90k;
                consumedSamples = 0;
                consumedSampleRate = 0;
            }
            offset = nextOffset;
            continue;
        }

        if (consumedSampleRate == 0) {
            consumedSampleRate = info.sampleRate;
        } else if (info.sampleRate != consumedSampleRate) {
            basePts90k += samplesTo90k(consumedSamples, consumedSampleRate);
            consumedSamples = 0;
            consumedSampleRate = info.sampleRate;
            if (m_audioDecoder) {
                m_audioDecoder->reset();
            }
        }

        const qint64 framePts90k = basePts90k + samplesTo90k(consumedSamples, consumedSampleRate);
        const int64_t sourcePtsMs = sourcePtsMsForAudio(framePts90k);
        if (sourcePtsMs >= 0) {
            const QByteArray frame = bytes.mid(offset, info.frameSize);
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

        consumedSamples += info.samplesPerFrame;
        offset += info.frameSize;
    }
}

int64_t NativeSrtIngestSession::sourcePtsMsForUnit(const CompressedAccessUnit& unit) {
    const int64_t rawDts90k = unit.dts90k >= 0 ? unit.dts90k : unit.pts90k;
    const int64_t rawPts90k = unit.pts90k >= 0 ? unit.pts90k : rawDts90k;
    if (rawDts90k < 0 || rawPts90k < 0) {
        return -1;
    }
    const int64_t unitDts90k = unwrapVideo90k(rawDts90k);
    const int64_t unitPts90k = rawPts90k + (unitDts90k - rawDts90k);

    // Video is the re-anchor authority: a big DTS jump => stream discontinuity =>
    // drop the shared anchor so the next PCR/PES re-establishes it (backstop to the
    // PCR discontinuity_indicator handled in processReceivedBytes).
    bool discontinuity = false;
    if (m_clock->locked() && m_prevDts90k >= 0) {
        const int64_t delta90k = unitDts90k - m_prevDts90k;
        if (delta90k > kForwardJump90k || delta90k < kBackwardTolerance90k) {
            log(QStringLiteral("Native SRT DTS discontinuity (%1 ms jump). Re-anchoring.")
                    .arg(delta90k / 90));
            discontinuity = true;
        }
    }
    m_prevDts90k = unitDts90k;

    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    m_clock->observe(unitDts90k, nowMs, discontinuity, ClockObservationRole::Authority);
    return m_clock->toSessionMs(unitPts90k);
}

int64_t NativeSrtIngestSession::sourcePtsMsFromAnchor(qint64 pts90k, int64_t anchorTs90k,
                                                      int64_t anchorStreamMs) {
    if (anchorTs90k < 0 || anchorStreamMs < 0 || pts90k < 0) {
        return -1;
    }
    return anchorStreamMs + ((pts90k - anchorTs90k) / 90);
}

int64_t NativeSrtIngestSession::unwrapPcr90k(int64_t raw90k) {
    return unwrap33Bit90k(raw90k, &m_prevRawPcr90k, &m_pcrWrapOffset90k);
}

int64_t NativeSrtIngestSession::unwrapVideo90k(int64_t raw90k) {
    return unwrap33Bit90k(raw90k, &m_prevRawVideoDts90k, &m_videoWrapOffset90k);
}

int64_t NativeSrtIngestSession::unwrapAudio90k(int64_t raw90k) {
    return unwrap33Bit90k(raw90k, &m_prevRawAudioPts90k, &m_audioWrapOffset90k);
}

int64_t NativeSrtIngestSession::sourcePtsMsForAudio(qint64 pts90k) {
    if (pts90k < 0) {
        return -1;
    }
    pts90k = unwrapAudio90k(pts90k);

    // Audio does NOT own the anchor (that independent anchor was the lip-sync bug).
    // Detect an audio discontinuity only to flush the decoder; timing uses the
    // shared anchor owned by the PCR/video path.
    if (m_clock->locked() && m_prevAudioPts90k >= 0) {
        const int64_t delta90k = pts90k - m_prevAudioPts90k;
        if (delta90k > kForwardJump90k || delta90k < kBackwardTolerance90k) {
            log(QStringLiteral("Native SRT audio PTS discontinuity (%1 ms jump). Flushing.")
                    .arg(delta90k / 90));
            if (m_audioDecoder) {
                m_audioDecoder->reset();
            }
        }
    }
    m_prevAudioPts90k = pts90k;

    // First-PES fallback: if audio arrives before any PCR or video, it establishes
    // the shared anchor (first-of-either); otherwise it maps against the existing one.
    const int64_t nowMs = m_callbacks.recordingClockMs ? m_callbacks.recordingClockMs() : -1;
    m_clock->observe(pts90k, nowMs, false, ClockObservationRole::Follower);
    int64_t sourcePtsMs = m_clock->toSessionMs(pts90k);
    // Safety clamp (no re-anchor): never stamp audio absurdly ahead of the clock.
    if (sourcePtsMs >= 0 && nowMs >= 0 && sourcePtsMs > nowMs + 10000) {
        sourcePtsMs = nowMs;
    }
    return sourcePtsMs;
}
