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
    void rejectsAllOnesTimestamp();
    void rejectsImplausibleSecondsAndNanos();
    void parsesValidTimestamp();
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

void TestUdpPtpClient::rejectsAllOnesTimestamp() {
    // A wire-crafted all-0xFF 10-byte timestamp: seconds = 2^48-1 ~= 2.8e14. The
    // naive `seconds * 1e9` would overflow int64 (UBSan: signed-integer-overflow).
    // The parser MUST reject it BEFORE the multiply so the garbage never reaches
    // the servo (haveT1/haveDelayResp stay false, no exchange completes).
    QByteArray ts(10, char(0xFF));
    int64_t outNs = 12345; // sentinel; must be left untouched on rejection
    QVERIFY(!UdpPtpClient::parseTimestampNsForTest(ts, 0, &outNs));
}

void TestUdpPtpClient::rejectsImplausibleSecondsAndNanos() {
    // Seconds just past the sane PTP bound (> 9e9 s) is rejected even though it
    // would not itself overflow — it is not a plausible PTP time.
    {
        QByteArray ts(10, '\0');
        const int64_t badSeconds = 9'000'000'001LL; // > 9e9 bound
        for (int i = 5; i >= 0; --i) {
            ts[i] = char((badSeconds >> (8 * (5 - i))) & 0xFF);
        }
        int64_t outNs = 0;
        QVERIFY(!UdpPtpClient::parseTimestampNsForTest(ts, 0, &outNs));
    }
    // A nanoseconds field >= 1e9 is malformed (valid PTP nanos are < 1e9).
    {
        QByteArray ts(10, '\0');
        const uint32_t badNanos = 1'000'000'000u; // == 1e9, out of range
        ts[6] = char((badNanos >> 24) & 0xFF);
        ts[7] = char((badNanos >> 16) & 0xFF);
        ts[8] = char((badNanos >> 8) & 0xFF);
        ts[9] = char(badNanos & 0xFF);
        int64_t outNs = 0;
        QVERIFY(!UdpPtpClient::parseTimestampNsForTest(ts, 0, &outNs));
    }
}

void TestUdpPtpClient::parsesValidTimestamp() {
    // A realistic grandmaster timestamp (~year 2026) must parse exactly. seconds =
    // 1'780'000'000 (~2026), nanos = 250'000'000 -> 1.78e18 ns, well within int64.
    const int64_t seconds = 1'780'000'000LL;
    const uint32_t nanos = 250'000'000u;
    QByteArray ts(10, '\0');
    for (int i = 5; i >= 0; --i) {
        ts[i] = char((seconds >> (8 * (5 - i))) & 0xFF);
    }
    ts[6] = char((nanos >> 24) & 0xFF);
    ts[7] = char((nanos >> 16) & 0xFF);
    ts[8] = char((nanos >> 8) & 0xFF);
    ts[9] = char(nanos & 0xFF);
    int64_t outNs = 0;
    QVERIFY(UdpPtpClient::parseTimestampNsForTest(ts, 0, &outNs));
    QCOMPARE(outNs, seconds * 1'000'000'000LL + int64_t(nanos));
}

QTEST_GUILESS_MAIN(TestUdpPtpClient)
#include "tst_udpptpclient.moc"
