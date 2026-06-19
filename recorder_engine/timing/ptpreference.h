#ifndef PTPREFERENCE_H
#define PTPREFERENCE_H
#include "iptpclient.h"
#include "ptpservo.h"
#include "timingreference.h"
#include <QString>
#include <atomic>
#include <memory>
#include <thread>

// ST 2059 / IEEE 1588 slave as the TimingReference top tier. Runs a background
// thread pulling exchanges from the IPtpClient into a PtpServo; nowSessionNs()
// returns disciplined master/facility ns once locked, else falls back to the
// local monotonic (so the pipeline never stalls waiting for PTP lock). Testable
// without sockets: the IPtpClient is injected (a fake serves scripted exchanges).
class PtpReference final : public TimingReference {
public:
    PtpReference(std::unique_ptr<IPtpClient> client, const QString& domainOrIface);
    PtpReference(std::unique_ptr<IPtpClient> client, const QString& domainOrIface,
                 int64_t sessionEpochMasterNs); // DI epoch for tests
    ~PtpReference() override;

    bool start(); // starts the client + the discipline thread
    void stop();

    int64_t nowSessionNs() const override;
    ReferenceTier tier() const override { return ReferenceTier::Ptp; }
    bool isExternal() const override { return m_locked.load(std::memory_order_acquire); }

    bool locked() const { return m_locked.load(std::memory_order_acquire); }

private:
    void disciplineLoop();

    // After this many consecutive lost/invalid exchanges the PTP feed is treated as
    // dropped: the servo is reset so locked()/isExternal() degrade to false and
    // nowSessionNs() falls back to local monotonic (no stale offset).
    static constexpr int kLossThreshold = 8;
    static constexpr int kPollTimeoutMs = 50;

    std::unique_ptr<IPtpClient> m_client;
    QString m_domain;
    PtpServo m_servo;
    std::atomic<int64_t> m_offsetNs{0};
    std::atomic<int64_t> m_epochMasterNs{
        -1};                    // master ns at session start (-1 = capture on first lock)
    bool m_epochFromDi = false; // epoch DI-provided (don't recapture)
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
#endif // PTPREFERENCE_H
