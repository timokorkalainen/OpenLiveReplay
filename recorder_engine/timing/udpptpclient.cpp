#include "udpptpclient.h"

#include <QDeadlineTimer>
#include <QNetworkInterface>
#include <QUdpSocket>

#include <chrono>

namespace {

// PTP transport (UDP/IPv4, Annex C/D): primary multicast group + event/general
// ports. Event messages (Sync, Delay_Req) carry the precise timestamps; general
// messages (Follow_Up, Delay_Resp) carry the master's timestamp fields.
const QHostAddress kPtpMulticast{QStringLiteral("224.0.1.129")};
constexpr quint16 kEventPort = 319;
constexpr quint16 kGeneralPort = 320;

// IEEE 1588 messageType nibble (low nibble of byte 0).
constexpr quint8 kMsgSync = 0x0;
constexpr quint8 kMsgDelayReq = 0x1;
constexpr quint8 kMsgFollowUp = 0x8;
constexpr quint8 kMsgDelayResp = 0x9;

// Common PTPv2 header is 34 bytes; the originTimestamp/receiveTimestamp field is
// a 10-byte structure (48-bit seconds + 32-bit nanoseconds) at offset 34.
constexpr int kHeaderLen = 34;
constexpr int kTimestampLen = 10;
constexpr int kMinMessageLen = kHeaderLen + kTimestampLen;

// Upper bound on a plausible PTP seconds field. The seconds field is a 48-bit
// attacker-controlled value (any LAN host can send a crafted Sync/Follow_Up/
// Delay_Resp), so an all-0xFF timestamp yields seconds ~= 2.8e14 and the naive
// `seconds * 1e9` overflows int64 (UBSan: signed-integer-overflow). 9e9 s is
// ~year 2255 — comfortably past any real session — and 9e9 * 1e9 = 9e18 stays
// below INT64_MAX (~9.22e18), so anything below this bound multiplies safely.
constexpr int64_t kMaxPlausibleSeconds = 9'000'000'000LL;
constexpr int64_t kNanosPerSecond = 1'000'000'000LL;

// Parse the 10-byte PTP timestamp at the given offset into absolute nanoseconds
// (PTP epoch). Returns false if the buffer is too short OR the field is
// implausible/malformed — seconds beyond a sane PTP range or nanos >= 1e9 — so a
// wire-crafted timestamp is REJECTED before the multiply (no signed overflow) and
// never reaches the servo (the caller leaves haveT1/haveDelayResp false, so no
// exchange completes).
bool parsePtpTimestampNs(const QByteArray& d, int offset, int64_t* outNs) {
    if (offset < 0 || offset + kTimestampLen > d.size()) {
        return false;
    }
    const auto* p = reinterpret_cast<const quint8*>(d.constData()) + offset;
    int64_t seconds = 0;
    for (int i = 0; i < 6; ++i) {
        seconds = (seconds << 8) | p[i];
    }
    int64_t nanos = 0;
    for (int i = 6; i < 10; ++i) {
        nanos = (nanos << 8) | p[i];
    }
    // Bounds-check BEFORE the multiply: a valid PTP nanoseconds field is < 1e9, and
    // a plausible seconds value is < kMaxPlausibleSeconds. Reject otherwise so the
    // int64 multiply below can never overflow.
    if (seconds > kMaxPlausibleSeconds || nanos >= kNanosPerSecond) {
        return false;
    }
    *outNs = seconds * kNanosPerSecond + nanos;
    return true;
}

quint8 messageType(const QByteArray& d) {
    if (d.isEmpty()) {
        return 0xFF;
    }
    return static_cast<quint8>(d.at(0)) & 0x0F;
}

} // namespace

struct UdpPtpClient::Impl {
    QUdpSocket* eventSocket = nullptr;
    QUdpSocket* generalSocket = nullptr;
    QHostAddress bindAddress = QHostAddress::AnyIPv4;
    QNetworkInterface joinInterface; // invalid = let the stack choose
    bool started = false;

    // The steady origin for localMonotonicNs(): a fixed point so the returned ns
    // are small, positive, and monotonic. Lock-free read (no shared mutable state
    // between threads beyond this const-after-construction value).
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();

    // Set once the sockets have been created/bound/joined on the discipline thread
    // (lazily, on the first nextExchange()). The sockets live wholly on that thread:
    // start() on the caller thread only stores config, never touches a QUdpSocket.
    bool socketsReady = false;

    // In-flight exchange assembly (only touched on the discipline thread inside
    // nextExchange(), never from localMonotonicNs()).
    int64_t pendingT1 = 0;          // master Sync egress (from Sync one-step or Follow_Up)
    int64_t pendingT2 = 0;          // local Sync ingress (SW timestamp)
    int64_t pendingT3 = 0;          // local Delay_Req egress (SW timestamp)
    int64_t pendingDelayRespT4 = 0; // master Delay_Req ingress (from Delay_Resp)
    bool haveSync = false;          // saw a Sync whose t2 we stamped
    bool haveT1 = false;            // t1 resolved (one-step Sync or its Follow_Up)
    bool delayReqSent = false;
    bool haveDelayResp = false; // t4 resolved (Delay_Resp received)
};

UdpPtpClient::UdpPtpClient() : m_d(std::make_unique<Impl>()) {}

UdpPtpClient::~UdpPtpClient() {
    stop();
}

bool UdpPtpClient::start(const QString& domainOrIface) {
    if (m_d->started) {
        return true;
    }

    // THREAD AFFINITY: start() runs on the CALLER thread (ReplayManager), but every
    // socket op (bind/join/read/write) must run on the DISCIPLINE thread, since Qt
    // sockets are not thread-safe and carry thread affinity. So start() ONLY records
    // config + marks started; the sockets are created lazily in nextExchange() on
    // the discipline thread (ensureSockets()). bind/join failure stays non-fatal
    // (degrade to local fallback), so start() still reports true as before.
    //
    // domainOrIface may name a local interface (by address or name) to join the
    // multicast on; a bare domain number (e.g. "0") means "any interface".
    if (!domainOrIface.isEmpty()) {
        const QHostAddress asAddr(domainOrIface);
        if (!asAddr.isNull()) {
            m_d->bindAddress = asAddr;
        } else {
            const QNetworkInterface iface = QNetworkInterface::interfaceFromName(domainOrIface);
            if (iface.isValid()) {
                m_d->joinInterface = iface;
            }
        }
    }

    m_d->started = true;
    return m_d->started;
}

void UdpPtpClient::ensureSockets() {
    // Runs on the discipline thread (first nextExchange()). Create+bind+join here so
    // the sockets are created, used, and destroyed all on one thread.
    if (m_d->socketsReady) {
        return;
    }
    m_d->socketsReady = true; // attempt once; a bind/join failure degrades, never retries

    m_d->eventSocket = new QUdpSocket();
    m_d->generalSocket = new QUdpSocket();

    // ShareAddress + ReuseAddressHint: multiple PTP listeners (and other apps) may
    // share the well-known ports; bind to AnyIPv4 then join the multicast group.
    const auto bindOpts = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
    const bool eventBound = m_d->eventSocket->bind(QHostAddress::AnyIPv4, kEventPort, bindOpts);
    const bool generalBound =
        m_d->generalSocket->bind(QHostAddress::AnyIPv4, kGeneralPort, bindOpts);

    if (eventBound) {
        m_d->joinInterface.isValid()
            ? m_d->eventSocket->joinMulticastGroup(kPtpMulticast, m_d->joinInterface)
            : m_d->eventSocket->joinMulticastGroup(kPtpMulticast);
    }
    if (generalBound) {
        m_d->joinInterface.isValid()
            ? m_d->generalSocket->joinMulticastGroup(kPtpMulticast, m_d->joinInterface)
            : m_d->generalSocket->joinMulticastGroup(kPtpMulticast);
    }
    // A failure to bind/join is not fatal: the client simply never completes an
    // exchange, so PtpReference stays in local fallback. The sockets are kept (even
    // unbound) so teardown is uniform; nextExchange() just never sees datagrams.
}

void UdpPtpClient::stop() {
    // Idempotent. The sockets are owned by the discipline thread, so the normal
    // teardown is the discipline thread calling stop() at the end of its loop (see
    // PtpReference::disciplineLoop). The destructor also calls stop() as a safety
    // net — a no-op when the sockets were never created (sockets stay null).
    if (m_d->eventSocket) {
        m_d->eventSocket->close();
        delete m_d->eventSocket;
        m_d->eventSocket = nullptr;
    }
    if (m_d->generalSocket) {
        m_d->generalSocket->close();
        delete m_d->generalSocket;
        m_d->generalSocket = nullptr;
    }
    m_d->socketsReady = false;
    m_d->haveSync = false;
    m_d->haveT1 = false;
    m_d->delayReqSent = false;
    m_d->started = false;
}

bool UdpPtpClient::parseTimestampNsForTest(const QByteArray& d, int offset, int64_t* outNs) {
    return parsePtpTimestampNs(d, offset, outNs);
}

int64_t UdpPtpClient::localMonotonicNs() const {
    // Lock-free: a pure steady_clock read against a const-after-construction
    // origin. No shared mutable state with nextExchange(), honoring the
    // IPtpClient thread-safety contract.
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_d->origin).count();
}

void UdpPtpClient::sendDelayReq() {
    if (!m_d->eventSocket) {
        return;
    }
    // Minimal PTPv2 Delay_Req: a header with messageType=Delay_Req and a zeroed
    // originTimestamp (the master replies with its receive time = t4). We do not
    // need a fully conformant header for the master to respond in typical SW-slave
    // setups; t3 is software-stamped at the moment of send.
    QByteArray msg(kMinMessageLen, '\0');
    msg[0] = static_cast<char>((0x0 << 4) | kMsgDelayReq); // majorSdoId=0 | type
    msg[1] = static_cast<char>(0x02);                      // versionPTP = 2
    msg[2] = static_cast<char>((kMinMessageLen >> 8) & 0xFF);
    msg[3] = static_cast<char>(kMinMessageLen & 0xFF);

    m_d->pendingT3 = localMonotonicNs();
    m_d->delayReqSent = true;
    m_d->eventSocket->writeDatagram(msg, kPtpMulticast, kEventPort);
}

void UdpPtpClient::processEventDatagram(const QByteArray& data) {
    if (data.size() < kHeaderLen) {
        return;
    }
    const quint8 type = messageType(data);
    if (type == kMsgSync) {
        // Stamp Sync ingress (t2) immediately. A one-step Sync carries t1 in its
        // originTimestamp; a two-step Sync's t1 arrives in the Follow_Up.
        m_d->pendingT2 = localMonotonicNs();
        m_d->haveSync = true;
        m_d->delayReqSent = false;
        int64_t t1 = 0;
        // Heuristic: a non-zero originTimestamp in the Sync itself is a one-step
        // master; otherwise wait for the Follow_Up (two-step).
        if (parsePtpTimestampNs(data, kHeaderLen, &t1) && t1 != 0) {
            m_d->pendingT1 = t1;
            m_d->haveT1 = true;
        } else {
            m_d->haveT1 = false;
        }
        // Kick off the reverse measurement: send Delay_Req and stamp t3.
        sendDelayReq();
    }
}

void UdpPtpClient::processGeneralDatagram(const QByteArray& data) {
    if (data.size() < kHeaderLen) {
        return;
    }
    const quint8 type = messageType(data);
    if (type == kMsgFollowUp) {
        int64_t t1 = 0;
        if (m_d->haveSync && parsePtpTimestampNs(data, kHeaderLen, &t1)) {
            m_d->pendingT1 = t1;
            m_d->haveT1 = true;
        }
    } else if (type == kMsgDelayResp) {
        // Delay_Resp's receiveTimestamp (t4) is the first 10-byte timestamp after
        // the header; the requestingPortIdentity that follows is not needed here.
        int64_t t4 = 0;
        if (parsePtpTimestampNs(data, kHeaderLen, &t4)) {
            m_d->pendingDelayRespT4 = t4;
            m_d->haveDelayResp = true;
        }
    }
}

PtpExchange UdpPtpClient::nextExchange(int timeoutMs) {
    if (!m_d->started) {
        return PtpExchange{};
    }

    // Create+bind+join on the discipline thread the first time through, so the
    // sockets' entire lifecycle (create/use/destroy) stays on this thread.
    ensureSockets();

    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        bool progressed = false;

        // Drain the event port (Sync). Sync triggers t2 + Delay_Req(t3).
        while (m_d->eventSocket && m_d->eventSocket->hasPendingDatagrams()) {
            QByteArray buf(static_cast<int>(m_d->eventSocket->pendingDatagramSize()), '\0');
            m_d->eventSocket->readDatagram(buf.data(), buf.size());
            processEventDatagram(buf);
            progressed = true;
        }
        // Drain the general port (Follow_Up -> t1, Delay_Resp -> t4).
        while (m_d->generalSocket && m_d->generalSocket->hasPendingDatagrams()) {
            QByteArray buf(static_cast<int>(m_d->generalSocket->pendingDatagramSize()), '\0');
            m_d->generalSocket->readDatagram(buf.data(), buf.size());
            processGeneralDatagram(buf);
            progressed = true;
        }

        // A complete two-way exchange needs all four timestamps: t1 (Sync/
        // Follow_Up), t2 (Sync ingress), t3 (Delay_Req egress), t4 (Delay_Resp).
        if (m_d->haveSync && m_d->haveT1 && m_d->delayReqSent && m_d->haveDelayResp) {
            PtpExchange ex;
            ex.t1 = m_d->pendingT1;
            ex.t2 = m_d->pendingT2;
            ex.t3 = m_d->pendingT3;
            ex.t4 = m_d->pendingDelayRespT4;
            ex.valid = true;
            // Reset for the next round.
            m_d->haveSync = false;
            m_d->haveT1 = false;
            m_d->delayReqSent = false;
            m_d->haveDelayResp = false;
            return ex;
        }

        if (!progressed) {
            // Nothing pending: wait on either socket for the remaining budget.
            const int remaining = static_cast<int>(deadline.remainingTime());
            if (remaining <= 0) {
                break;
            }
            const int slice = remaining < 10 ? remaining : 10;
            bool any = false;
            if (m_d->eventSocket) {
                any = m_d->eventSocket->waitForReadyRead(slice);
            }
            if (!any && m_d->generalSocket) {
                m_d->generalSocket->waitForReadyRead(1);
            }
            // Either path slept up to the slice; loop again until the deadline.
        }
    }

    // Timed out (e.g. no grandmaster): an INVALID exchange so the reference
    // degrades to local time and never falsely locks.
    return PtpExchange{};
}
