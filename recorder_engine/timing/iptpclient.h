#ifndef IPTPCLIENT_H
#define IPTPCLIENT_H
#include <cstdint>

// QString is only referenced by the backend interface's signatures; forward-
// declared so PtpServo (which includes this header solely for PtpExchange) stays
// Qt-free. The real UDP backend (Task 5) and the test fake include <QString>.
class QString;

// One PTP two-way exchange sample: the four IEEE 1588 timestamps (ns).
//   t1 = master Sync egress (from Sync/Follow_Up)
//   t2 = slave Sync ingress (local)
//   t3 = slave Delay_Req egress (local)
//   t4 = master Delay_Resp ingress (from Delay_Resp)
struct PtpExchange {
    int64_t t1 = 0;
    int64_t t2 = 0;
    int64_t t3 = 0;
    int64_t t4 = 0;
    bool valid = false;
};

// The socket/timestamp backend behind a PTP slave. Pure interface (no Qt types in
// the body, no sockets): the real impl is a QUdpSocket client on UDP 319/320
// (Task 5); tests inject a fake returning a scripted exchange stream. Mirrors the
// INdiReceiverBackend DI pattern so PtpServo/PtpReference are unit-testable.
class IPtpClient {
public:
    virtual ~IPtpClient() = default;
    virtual bool start(const QString& domainOrIface) = 0;
    virtual void stop() = 0;
    // Block up to timeoutMs for the next completed two-way exchange. Called only
    // from PtpReference's discipline thread.
    virtual PtpExchange nextExchange(int timeoutMs) = 0;
    // THREAD-SAFETY CONTRACT: must be safe to call concurrently with nextExchange().
    // PtpReference reads the local clock from the caller thread (nowSessionNs) while
    // its discipline thread is blocked in/around nextExchange() — implementations
    // (the real UdpPtpClient included) MUST make this read lock-free or synchronized.
    virtual int64_t localMonotonicNs() const = 0;
};
#endif // IPTPCLIENT_H
