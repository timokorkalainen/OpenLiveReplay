#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"

class TestSourceHealth : public QObject {
    Q_OBJECT
private slots:
    void quietLinkIsGreen();
    void unrecoveredDropIsRed();
    void dropBeatsHighRetrans();
    void elevatedRetransIsAmber();
    void lightRetransIsGreen();
    void thresholdIsStrict();
    void counterResetIsGreen();
    void noRecvNoAmber();
};

// helper: build a cumulative snapshot
static IngestStats snap(qint64 recv, qint64 retrans, qint64 loss, qint64 drop) {
    IngestStats s;
    s.kind = IngestStatsKind::Srt;
    s.recvTotal = recv;
    s.retransTotal = retrans;
    s.lossTotal = loss;
    s.dropTotal = drop;
    return s;
}

void TestSourceHealth::quietLinkIsGreen() {
    const IngestStats a = snap(1000, 0, 0, 0);
    const IngestStats b = snap(2000, 0, 0, 0); // +1000 recv, no retrans/drop
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Green);
}

void TestSourceHealth::unrecoveredDropIsRed() {
    const IngestStats a = snap(1000, 10, 20, 0);
    const IngestStats b = snap(2000, 15, 25, 3); // +3 unrecovered drops this window
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Red);
}

void TestSourceHealth::dropBeatsHighRetrans() {
    const IngestStats a = snap(1000, 0, 0, 0);
    const IngestStats b = snap(2000, 900, 900, 1); // huge retrans AND a drop -> Red
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Red);
}

void TestSourceHealth::elevatedRetransIsAmber() {
    const IngestStats a = snap(1000, 0, 0, 0);
    const IngestStats b = snap(2000, 30, 30, 0); // 30/1000 = 3% > 2%, no drop
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Amber);
}

void TestSourceHealth::lightRetransIsGreen() {
    const IngestStats a = snap(1000, 0, 0, 0);
    const IngestStats b = snap(2000, 10, 10, 0); // 10/1000 = 1% < 2%
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Green);
}

void TestSourceHealth::thresholdIsStrict() {
    const IngestStats a = snap(1000, 0, 0, 0);
    const IngestStats b = snap(2000, 20, 20, 0); // exactly 2% -> NOT amber (strictly >)
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Green);
}

void TestSourceHealth::counterResetIsGreen() {
    // After a reconnect the socket's cumulative counters restart from 0, so the
    // "current" snapshot is smaller than the previous -> negative deltas. Must clamp
    // to Green, never misread a reset as recovery-success-with-loss.
    const IngestStats a = snap(5000, 400, 800, 9);
    const IngestStats b = snap(100, 2, 2, 0);
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Green);
}

void TestSourceHealth::noRecvNoAmber() {
    // No packets received this window but retrans counter moved: the rate is
    // undefined (div-by-zero guarded by dRecv>0), so it must stay Green, not Amber.
    const IngestStats a = snap(1000, 10, 10, 0);
    const IngestStats b = snap(1000, 15, 15, 0); // dRecv==0, dRetrans>0, no drop
    QCOMPARE(srtHealth(a, b, 0.02), SourceHealth::Green);
}

QTEST_GUILESS_MAIN(TestSourceHealth)
#include "tst_srt_health.moc"
