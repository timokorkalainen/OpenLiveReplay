#include <QtTest>

#include "recorder_engine/timing/ptpreference.h"
#include "recorder_engine/timing/iptpclient.h"

#include <atomic>
#include <mutex>

// Fake IPtpClient mirroring the FakeNdiReceiverBackend DI pattern: it serves a
// scripted stream of two-way exchanges (each with a known local->master offset)
// and exposes a test-controllable localMonotonicNs(). nextExchange() blocks
// briefly when the script is drained so the discipline thread does not busy-spin,
// and a "loss" mode makes it return invalid exchanges (timeouts) so the PTP-loss
// degrade path can be exercised. Thread-safe: the discipline loop runs on its own
// thread while the test drives the script.
class FakePtpClient final : public IPtpClient {
public:
    explicit FakePtpClient(int64_t offsetO, int64_t delayD) : m_offset(offsetO), m_delay(delayD) {}

    bool start(const QString&) override {
        m_started.store(true, std::memory_order_release);
        return true;
    }
    void stop() override { m_started.store(false, std::memory_order_release); }

    // Build a valid IEEE 1588 exchange anchored at a synthetic master egress time
    // t1 with the configured symmetric path delay D and offset O, so the servo
    // recovers offset==O and meanPathDelay==D exactly.
    PtpExchange nextExchange(int timeoutMs) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_loss.load(std::memory_order_acquire)) {
                return PtpExchange{}; // invalid -> a timeout / lost exchange
            }
            const int64_t t1 = m_seq * 1'000'000LL;
            const int64_t t3 = t1 + 500'000LL;
            ++m_seq;
            PtpExchange ex;
            ex.t1 = t1;
            ex.t2 = t1 + m_delay + m_offset;
            ex.t3 = t3;
            ex.t4 = t3 + m_delay - m_offset;
            ex.valid = true;
            ++m_served;
            return ex;
        }
        Q_UNUSED(timeoutMs);
    }

    int64_t localMonotonicNs() const override { return m_localNs.load(std::memory_order_acquire); }

    // --- test controls ---
    void setLocalNs(int64_t ns) { m_localNs.store(ns, std::memory_order_release); }
    void setLoss(bool lost) { m_loss.store(lost, std::memory_order_release); }
    int served() const { return m_served.load(std::memory_order_acquire); }
    bool started() const { return m_started.load(std::memory_order_acquire); }
    int64_t offset() const { return m_offset; }

private:
    std::mutex m_mutex;
    int64_t m_offset;
    int64_t m_delay;
    int64_t m_seq = 0;
    std::atomic<int64_t> m_localNs{0};
    std::atomic<int> m_served{0};
    std::atomic<bool> m_loss{false};
    std::atomic<bool> m_started{false};
};

class TestPtpReference : public QObject {
    Q_OBJECT
private slots:
    void tierIsAlwaysPtp();
    void notExternalBeforeLock();
    void fallsBackToLocalBeforeLock();
    void locksAndReportsExternal();
    void nowSessionTracksMasterMinusEpoch();
    void degradesToLocalOnPtpLoss();
    void stopJoinsAndStopsClient();
};

void TestPtpReference::tierIsAlwaysPtp() {
    PtpReference ref(std::make_unique<FakePtpClient>(4000, 60000), QStringLiteral("0"),
                     /*epochMasterNs*/ 0);
    QCOMPARE(ref.tier(), ReferenceTier::Ptp);
}

void TestPtpReference::notExternalBeforeLock() {
    // No thread started: the servo has never observed an exchange, so the reference
    // is not yet external.
    PtpReference ref(std::make_unique<FakePtpClient>(4000, 60000), QStringLiteral("0"), 0);
    QVERIFY(!ref.isExternal());
    QVERIFY(!ref.locked());
}

void TestPtpReference::fallsBackToLocalBeforeLock() {
    auto client = std::make_unique<FakePtpClient>(4000, 60000);
    FakePtpClient* raw = client.get();
    const int64_t E = 1'000'000'000LL;  // DI session epoch in master ns
    raw->setLocalNs(E + 250'000'000LL); // 250 ms of local time past the epoch
    PtpReference ref(std::move(client), QStringLiteral("0"), E);

    // Before lock nowSessionNs() falls back to local-monotonic-since-epoch: never
    // negative, and equal to local - epoch (no offset applied while unlocked).
    QVERIFY(!ref.isExternal());
    QCOMPARE(ref.nowSessionNs(), 250'000'000LL);
    QVERIFY(ref.nowSessionNs() >= 0);
}

void TestPtpReference::locksAndReportsExternal() {
    auto client = std::make_unique<FakePtpClient>(4000, 60000);
    PtpReference ref(std::move(client), QStringLiteral("0"), 0);
    QVERIFY(ref.start());
    // The discipline thread pumps scripted exchanges into the servo; after enough
    // of them the servo locks and the reference reports external + Ptp.
    QTRY_VERIFY_WITH_TIMEOUT(ref.locked(), 2000);
    QVERIFY(ref.isExternal());
    QCOMPARE(ref.tier(), ReferenceTier::Ptp);
    ref.stop();
}

void TestPtpReference::nowSessionTracksMasterMinusEpoch() {
    const int64_t O = 4000;
    const int64_t E = 2'000'000'000LL; // DI epoch (master ns at session start)
    auto client = std::make_unique<FakePtpClient>(O, 60000);
    FakePtpClient* raw = client.get();
    raw->setLocalNs(E + 500'000'000LL); // local clock is 500 ms past the epoch
    PtpReference ref(std::move(client), QStringLiteral("0"), E);
    QVERIFY(ref.start());
    QTRY_VERIFY_WITH_TIMEOUT(ref.locked(), 2000);

    // Once locked, nowSessionNs == (local - offset) - epoch, i.e. master/facility
    // time relative to the session epoch. local==E+500ms, offset==O, epoch==E ->
    // 500ms - O.
    const int64_t localNs = raw->localMonotonicNs();
    QCOMPARE(ref.nowSessionNs(), (localNs - O) - E);
    QVERIFY(qAbs(ref.nowSessionNs() - (500'000'000LL - O)) <= 1);
    ref.stop();
}

void TestPtpReference::degradesToLocalOnPtpLoss() {
    const int64_t O = 4000;
    const int64_t E = 0;
    auto client = std::make_unique<FakePtpClient>(O, 60000);
    FakePtpClient* raw = client.get();
    raw->setLocalNs(E + 300'000'000LL);
    PtpReference ref(std::move(client), QStringLiteral("0"), E);
    QVERIFY(ref.start());
    QTRY_VERIFY_WITH_TIMEOUT(ref.locked(), 2000);
    QVERIFY(ref.isExternal());

    // PTP drops: the client stops yielding valid exchanges (timeouts). The
    // reference must degrade gracefully -> external goes false and nowSessionNs
    // falls back to local-monotonic-since-epoch (no stale offset), never stalling.
    raw->setLoss(true);
    QTRY_VERIFY_WITH_TIMEOUT(!ref.isExternal(), 2000);
    QVERIFY(!ref.locked());
    QCOMPARE(ref.nowSessionNs(), 300'000'000LL); // local - epoch, offset dropped
    ref.stop();
}

void TestPtpReference::stopJoinsAndStopsClient() {
    auto client = std::make_unique<FakePtpClient>(4000, 60000);
    FakePtpClient* raw = client.get();
    PtpReference ref(std::move(client), QStringLiteral("0"), 0);
    QVERIFY(ref.start());
    QTRY_VERIFY_WITH_TIMEOUT(raw->served() > 0, 2000);
    ref.stop();
    QVERIFY(!raw->started()); // stop() tore the client down
}

QTEST_GUILESS_MAIN(TestPtpReference)
#include "tst_ptpreference.moc"
