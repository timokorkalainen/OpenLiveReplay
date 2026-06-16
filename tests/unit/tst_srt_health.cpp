#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"

class TestSrtHealth : public QObject {
    Q_OBJECT
private slots:
    void quietLinkIsGreen();
    void unrecoveredDropIsRed();
    void dropBeatsHighRetrans();
    void elevatedRetransIsAmber();
    void lightRetransIsGreen();
    void thresholdIsStrict();
    void counterResetIsGreen();
};

// helper: build a cumulative snapshot
static SrtStats snap(qint64 recv, qint64 retrans, qint64 loss, qint64 drop) {
    SrtStats s;
    s.recvTotal = recv;
    s.retransTotal = retrans;
    s.lossTotal = loss;
    s.dropTotal = drop;
    return s;
}

void TestSrtHealth::quietLinkIsGreen() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 0, 0, 0); // +1000 recv, no retrans/drop
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

void TestSrtHealth::unrecoveredDropIsRed() {
    const SrtStats a = snap(1000, 10, 20, 0);
    const SrtStats b = snap(2000, 15, 25, 3); // +3 unrecovered drops this window
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Red);
}

void TestSrtHealth::dropBeatsHighRetrans() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 900, 900, 1); // huge retrans AND a drop -> Red
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Red);
}

void TestSrtHealth::elevatedRetransIsAmber() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 30, 30, 0); // 30/1000 = 3% > 2%, no drop
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Amber);
}

void TestSrtHealth::lightRetransIsGreen() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 10, 10, 0); // 10/1000 = 1% < 2%
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

void TestSrtHealth::thresholdIsStrict() {
    const SrtStats a = snap(1000, 0, 0, 0);
    const SrtStats b = snap(2000, 20, 20, 0); // exactly 2% -> NOT amber (strictly >)
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

void TestSrtHealth::counterResetIsGreen() {
    // After a reconnect the socket's cumulative counters restart from 0, so the
    // "current" snapshot is smaller than the previous -> negative deltas. Must clamp
    // to Green, never misread a reset as recovery-success-with-loss.
    const SrtStats a = snap(5000, 400, 800, 9);
    const SrtStats b = snap(100, 2, 2, 0);
    QCOMPARE(srtHealth(a, b, 0.02), SrtHealth::Green);
}

QTEST_GUILESS_MAIN(TestSrtHealth)
#include "tst_srt_health.moc"
