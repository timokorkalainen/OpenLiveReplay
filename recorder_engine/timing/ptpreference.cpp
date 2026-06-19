#include "ptpreference.h"

PtpReference::PtpReference(std::unique_ptr<IPtpClient> client, const QString& domainOrIface)
    : m_client(std::move(client)), m_domain(domainOrIface) {}

PtpReference::PtpReference(std::unique_ptr<IPtpClient> client, const QString& domainOrIface,
                           int64_t sessionEpochMasterNs)
    : m_client(std::move(client)), m_domain(domainOrIface), m_epochFromDi(true) {
    // A DI-provided epoch fixes the session origin in master/facility time: every
    // nowSessionNs() is measured relative to it, so the timeline is stable across
    // the unlocked->locked transition and is never re-captured at runtime.
    m_epochMasterNs.store(sessionEpochMasterNs, std::memory_order_release);
}

PtpReference::~PtpReference() {
    stop();
}

bool PtpReference::start() {
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }
    if (!m_client || !m_client->start(m_domain)) {
        return false;
    }
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&PtpReference::disciplineLoop, this);
    return true;
}

void PtpReference::stop() {
    // The IPtpClient's sockets are created/used/destroyed wholly on the discipline
    // thread (Qt sockets are not thread-safe / have thread affinity). So we DO NOT
    // call m_client->stop() here on the caller thread: we only signal the loop to
    // exit and join it. The loop itself calls m_client->stop() as its last act (see
    // disciplineLoop), so by the time join() returns the sockets are already torn
    // down on the thread that owns them. UdpPtpClient::~UdpPtpClient() still calls
    // stop() as an idempotent safety net (a no-op once the sockets are gone, or if
    // the loop never ran).
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void PtpReference::disciplineLoop() {
    int consecutiveLost = 0;
    while (m_running.load(std::memory_order_acquire)) {
        const PtpExchange ex = m_client->nextExchange(kPollTimeoutMs);
        if (ex.valid) {
            consecutiveLost = 0;
            m_servo.observe(ex);
            m_offsetNs.store(m_servo.offsetNs(), std::memory_order_release);
            const bool nowLocked = m_servo.locked();
            // Capture the session epoch from master time on the first lock unless an
            // epoch was DI-provided (tests / a pre-agreed facility origin).
            if (nowLocked && !m_epochFromDi &&
                m_epochMasterNs.load(std::memory_order_acquire) < 0) {
                const int64_t masterNs = m_client->localMonotonicNs() - m_servo.offsetNs();
                m_epochMasterNs.store(masterNs, std::memory_order_release);
            }
            m_locked.store(nowLocked, std::memory_order_release);
        } else {
            // A lost/timed-out exchange. A few are normal jitter; a sustained run is
            // PTP loss -> reset the servo so locked()/isExternal() degrade to false
            // and nowSessionNs() falls back to local monotonic (no stale offset).
            if (++consecutiveLost >= kLossThreshold) {
                m_servo.reset();
                m_offsetNs.store(0, std::memory_order_release);
                m_locked.store(false, std::memory_order_release);
                if (!m_epochFromDi) {
                    m_epochMasterNs.store(-1, std::memory_order_release);
                }
            }
        }
    }
    // Tear the client (and its sockets) down on the THREAD THAT OWNS THEM. The
    // sockets were created lazily inside nextExchange() on this discipline thread;
    // closing/deleting them here keeps their whole lifecycle on a single thread.
    // stop() joins this thread, so teardown is guaranteed complete before stop()
    // returns. stop() must NOT also call m_client->stop() (that would touch the
    // sockets from the caller thread).
    if (m_client) {
        m_client->stop();
    }
}

int64_t PtpReference::nowSessionNs() const {
    if (!m_client) {
        return 0;
    }
    const int64_t localNs = m_client->localMonotonicNs();
    // Once locked, map local monotonic -> master/facility time by removing the
    // disciplined offset; before lock (or after PTP loss) fall back to local time
    // so the pipeline keeps advancing and never stalls or goes negative.
    const int64_t masterNs = m_locked.load(std::memory_order_acquire)
                                 ? localNs - m_offsetNs.load(std::memory_order_acquire)
                                 : localNs;
    // Session ns since the epoch (the master ns at session start). A not-yet-captured
    // epoch (no DI, not yet locked) anchors at 0 so the timeline starts at local time.
    int64_t epoch = m_epochMasterNs.load(std::memory_order_acquire);
    if (epoch < 0) {
        epoch = 0;
    }
    return masterNs - epoch;
}
