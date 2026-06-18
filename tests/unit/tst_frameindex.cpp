#include <QtTest>
#include "playback/frameindex.h"

class TestFrameIndex : public QObject {
    Q_OBJECT
private slots:
    void emptyReturnsNullopt();
    void singleEntryExactAndBefore();
    void nearestPicksLargestAtOrBefore();
    void beforeFirstReturnsNullopt();
    void nonMonotonicAppendIsIgnored();
    void clearResets();
    void newestTracksGrowth();
};

void TestFrameIndex::emptyReturnsNullopt() {
    FrameIndex idx;
    QCOMPARE(idx.size(), 0);
    QVERIFY(!idx.nearestAtOrBefore(0).has_value());
    QVERIFY(!idx.nearestAtOrBefore(1000).has_value());
}

void TestFrameIndex::singleEntryExactAndBefore() {
    FrameIndex idx;
    idx.append(100, 4096);
    QCOMPARE(idx.size(), 1);
    QCOMPARE(idx.nearestAtOrBefore(100).value(), qint64(4096));
    QCOMPARE(idx.nearestAtOrBefore(150).value(), qint64(4096));
}

void TestFrameIndex::nearestPicksLargestAtOrBefore() {
    FrameIndex idx;
    idx.append(0, 0);
    idx.append(40, 1000);
    idx.append(80, 2000);
    idx.append(120, 3000);
    QCOMPARE(idx.nearestAtOrBefore(0).value(), qint64(0));
    QCOMPARE(idx.nearestAtOrBefore(39).value(), qint64(0));
    QCOMPARE(idx.nearestAtOrBefore(40).value(), qint64(1000));
    QCOMPARE(idx.nearestAtOrBefore(119).value(), qint64(2000));
    QCOMPARE(idx.nearestAtOrBefore(120).value(), qint64(3000));
    QCOMPARE(idx.nearestAtOrBefore(99999).value(), qint64(3000));
}

void TestFrameIndex::beforeFirstReturnsNullopt() {
    FrameIndex idx;
    idx.append(40, 1000);
    QVERIFY(!idx.nearestAtOrBefore(0).has_value());
    QVERIFY(!idx.nearestAtOrBefore(39).has_value());
}

void TestFrameIndex::nonMonotonicAppendIsIgnored() {
    FrameIndex idx;
    idx.append(100, 5000);
    idx.append(80, 4000);  // older PTS than tail -> ignored
    idx.append(100, 6000); // duplicate PTS -> ignored (first wins)
    QCOMPARE(idx.size(), 1);
    QCOMPARE(idx.nearestAtOrBefore(200).value(), qint64(5000));
}

void TestFrameIndex::clearResets() {
    FrameIndex idx;
    idx.append(10, 100);
    idx.append(20, 200);
    idx.clear();
    QCOMPARE(idx.size(), 0);
    QVERIFY(!idx.nearestAtOrBefore(20).has_value());
}

void TestFrameIndex::newestTracksGrowth() {
    FrameIndex idx;
    QVERIFY(!idx.newestPtsMs().has_value());
    idx.append(0, 0);
    QCOMPARE(idx.newestPtsMs().value(), qint64(0));
    idx.append(40, 1000);
    QCOMPARE(idx.newestPtsMs().value(), qint64(40));
    // Simulate file growth: query, then append more, then query again.
    QCOMPARE(idx.nearestAtOrBefore(40).value(), qint64(1000));
    idx.append(80, 2000);
    QCOMPARE(idx.nearestAtOrBefore(80).value(), qint64(2000));
    QCOMPARE(idx.newestPtsMs().value(), qint64(80));
}

QTEST_MAIN(TestFrameIndex)
#include "tst_frameindex.moc"
