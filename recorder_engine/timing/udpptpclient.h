#ifndef UDPPTPCLIENT_H
#define UDPPTPCLIENT_H

#include "iptpclient.h"

#include <QHostAddress>
#include <QString>
#include <cstdint>
#include <memory>

class QUdpSocket;

// The real IPtpClient backend: a SOFTWARE PTP (IEEE 1588 / ST 2059) slave over
// QUdpSocket. Joins the PTP multicast group 224.0.1.129 on the event (319) and
// general (320) ports, consumes Sync/Follow_Up (t1) and Delay_Resp (t4), and
// software-timestamps Sync ingress (t2) and Delay_Req egress (t3) from a
// monotonic clock — surfacing each completed two-way exchange via nextExchange().
//
// PRECISION CEILING — SOFTWARE TIMESTAMPS ONLY. t2/t3 are stamped in user space
// (steady_clock at the socket read/write), NOT by a NIC PHC. This is materially
// better than a clockless IP timeline but is NOT NIC-hardware-timestamped /
// genlock-grade: kernel + scheduling jitter bound the achievable offset accuracy
// to the tens-of-microseconds range. True genlock requires hardware timestamping
// (a future backend swap behind this same IPtpClient seam) or a genlocked
// capture path. This client reports honestly through PtpReference::tier()/
// isExternal() and never claims more precision than it has.
//
// RUNTIME-GATED: only constructed/started when PTP is opted in (ReplayManager's
// OLR_TIMING_PTP gate). Compiles everywhere (Qt Network is cross-platform); no
// platform #ifdef excludes it from any build.
class UdpPtpClient final : public IPtpClient {
public:
    UdpPtpClient();
    ~UdpPtpClient() override;

    // domainOrIface: a PTP domain number (e.g. "0") and/or a local interface
    // address/name to bind the multicast join to (empty = any interface).
    bool start(const QString& domainOrIface) override;
    void stop() override;

    // Blocks up to timeoutMs for the next completed two-way exchange. Called only
    // from PtpReference's discipline thread. Returns an INVALID exchange on
    // timeout (e.g. no grandmaster) so the reference degrades to local time.
    PtpExchange nextExchange(int timeoutMs) override;

    // THREAD-SAFE, LOCK-FREE: a steady_clock read with no shared mutable state,
    // safe to call concurrently with nextExchange() per the IPtpClient contract.
    int64_t localMonotonicNs() const override;

private:
    void processGeneralDatagram(const QByteArray& data);
    void processEventDatagram(const QByteArray& data);
    void sendDelayReq();

    struct Impl;
    std::unique_ptr<Impl> m_d;
};

#endif // UDPPTPCLIENT_H
