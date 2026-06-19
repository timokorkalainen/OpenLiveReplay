#include <QtTest>

#include "recorder_engine/timing/iptpclient.h"
#include "recorder_engine/timing/udpptpclient.h"

#include <thread>

// UdpPtpClient is the real QUdpSocket-backed IPtpClient (PTP slave on UDP
// 319/320). Its wire behavior against a real grandmaster cannot be exercised in
// a unit test (no grandmaster on the CI LAN); this suite verifies the contract
// the rest of the system relies on:
//   * it constructs and binds/joins the PTP multicast without crashing,
//   * localMonotonicNs() is monotonic and safe to call concurrently with
//     nextExchange() (the documented IPtpClient thread-safety contract), and
//   * with NO grandmaster present it degrades cleanly — nextExchange() times
//     out returning an INVALID exchange, so a PtpReference over it never locks
//     and stays in local fallback (no false "external" lock, no stall).
// Hardware / grandmaster validation is explicitly out of scope (documented in
// the plan: SW timestamps are the precision ceiling).
class TestUdpPtpClient : public QObject {
    Q_OBJECT
private slots:
    void constructsWithoutCrashing();
    void localMonotonicIsMonotonic();
    void localMonotonicIsValidBeforeStart();
    void degradesCleanlyWithoutGrandmaster();
    void localMonotonicSafeConcurrentlyWithNextExchange();
    void stopIsIdempotent();
};

void TestUdpPtpClient::constructsWithoutCrashing() {
    UdpPtpClient client;
    // start() may succeed or fail depending on the host's multicast support; it
    // must never crash and must be safe to call once.
    const bool started = client.start(QStringLiteral("0"));
    Q_UNUSED(started);
    client.stop();
    QVERIFY(true);
}

void TestUdpPtpClient::localMonotonicIsMonotonic() {
    UdpPtpClient client;
    const int64_t a = client.localMonotonicNs();
    QThread::msleep(5);
    const int64_t b = client.localMonotonicNs();
    QVERIFY(b >= a);
    QVERIFY(a >= 0);
}

void TestUdpPtpClient::localMonotonicIsValidBeforeStart() {
    // The contract is that localMonotonicNs() works regardless of start state —
    // PtpReference samples it for fallback before the discipline thread runs.
    UdpPtpClient client;
    QVERIFY(client.localMonotonicNs() >= 0);
}

void TestUdpPtpClient::degradesCleanlyWithoutGrandmaster() {
    UdpPtpClient client;
    client.start(QStringLiteral("0"));
    // No grandmaster on the test LAN -> no Sync/Follow_Up/Delay_Resp ever
    // completes a two-way exchange. nextExchange() must time out and return an
    // INVALID exchange (never a bogus "valid" sample that could fake a lock).
    const PtpExchange ex = client.nextExchange(50);
    QVERIFY(!ex.valid);
    client.stop();
}

void TestUdpPtpClient::localMonotonicSafeConcurrentlyWithNextExchange() {
    UdpPtpClient client;
    client.start(QStringLiteral("0"));

    std::atomic<bool> stop{false};
    std::atomic<int64_t> last{0};
    std::atomic<bool> monotonic{true};
    // Reader thread hammers localMonotonicNs() while the main thread blocks in
    // nextExchange() — mirrors PtpReference::nowSessionNs() racing the discipline
    // thread. Must stay monotonic and never crash (TSan/ASan catch races).
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t v = client.localMonotonicNs();
            int64_t prev = last.load(std::memory_order_acquire);
            if (v < prev) {
                monotonic.store(false, std::memory_order_release);
            }
            last.store(v, std::memory_order_release);
        }
    });

    for (int i = 0; i < 4; ++i) {
        const PtpExchange ex = client.nextExchange(20);
        QVERIFY(!ex.valid); // still no grandmaster
    }

    stop.store(true, std::memory_order_release);
    reader.join();
    QVERIFY(monotonic.load(std::memory_order_acquire));
    client.stop();
}

void TestUdpPtpClient::stopIsIdempotent() {
    UdpPtpClient client;
    client.start(QStringLiteral("0"));
    client.stop();
    client.stop(); // double-stop is a no-op, never a crash
    QVERIFY(true);
}

QTEST_GUILESS_MAIN(TestUdpPtpClient)
#include "tst_udpptpclient.moc"
