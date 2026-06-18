#include <QtTest>
#include <QJsonObject>
#include <QJsonArray>
#include "playback/replayplaylist.h"

class TestReplayPlaylist : public QObject {
    Q_OBJECT
private slots:
    void emptyHasNoEntries();
    void markInCreatesPendingEntry();
    void markOutClosesEntry();
    void markOutBeforeInIsRejected();
    void markOutWithoutInIsRejected();
    void recallOutOfRangeReturnsNullopt();
    void multipleOrderedEntries();
    void setSpeedClampsToPositive();
    void jsonRoundTripPreservesEntries();
    void fromJsonRejectsMalformed();
};

void TestReplayPlaylist::emptyHasNoEntries() {
    ReplayPlaylist p;
    QCOMPARE(p.count(), 0);
    QVERIFY(!p.recall(0).has_value());
}

void TestReplayPlaylist::markInCreatesPendingEntry() {
    ReplayPlaylist p;
    int idx = p.markIn("/clips/a.mkv", 1000);
    QCOMPARE(idx, 0);
    QCOMPARE(p.count(), 1);
    auto e = p.recall(0).value();
    QCOMPARE(e.clipPath, QString("/clips/a.mkv"));
    QCOMPARE(e.inMs, qint64(1000));
    QCOMPARE(e.outMs, qint64(-1)); // open
    QCOMPARE(e.speed, 1.0);
}

void TestReplayPlaylist::markOutClosesEntry() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 1000);
    QVERIFY(p.markOut(3000));
    auto e = p.recall(0).value();
    QCOMPARE(e.outMs, qint64(3000));
}

void TestReplayPlaylist::markOutBeforeInIsRejected() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 1000);
    QVERIFY(!p.markOut(500));
    QCOMPARE(p.recall(0).value().outMs, qint64(-1));
}

void TestReplayPlaylist::markOutWithoutInIsRejected() {
    ReplayPlaylist p;
    QVERIFY(!p.markOut(1000));
    QCOMPARE(p.count(), 0);
}

void TestReplayPlaylist::recallOutOfRangeReturnsNullopt() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    QVERIFY(!p.recall(-1).has_value());
    QVERIFY(!p.recall(1).has_value());
}

void TestReplayPlaylist::multipleOrderedEntries() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 1000);
    p.markOut(2000);
    int idx2 = p.markIn("/clips/b.mkv", 5000);
    p.markOut(6000);
    QCOMPARE(p.count(), 2);
    QCOMPARE(idx2, 1);
    QCOMPARE(p.recall(0).value().clipPath, QString("/clips/a.mkv"));
    QCOMPARE(p.recall(1).value().inMs, qint64(5000));
}

void TestReplayPlaylist::setSpeedClampsToPositive() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    p.markOut(1000);
    p.setSpeed(0, 0.5);
    QCOMPARE(p.recall(0).value().speed, 0.5);
    p.setSpeed(0, -2.0); // clamped to a small positive epsilon
    QVERIFY(p.recall(0).value().speed > 0.0);
}

void TestReplayPlaylist::jsonRoundTripPreservesEntries() {
    ReplayPlaylist a;
    a.markIn("/clips/a.mkv", 1000);
    a.markOut(2000);
    a.setSpeed(0, 0.5);
    a.markIn("/clips/b.mkv", 5000);
    a.markOut(6000);
    QJsonObject json = a.toJson();

    ReplayPlaylist b;
    QVERIFY(b.fromJson(json));
    QCOMPARE(b.count(), 2);
    QCOMPARE(b.recall(0).value().clipPath, QString("/clips/a.mkv"));
    QCOMPARE(b.recall(0).value().inMs, qint64(1000));
    QCOMPARE(b.recall(0).value().outMs, qint64(2000));
    QCOMPARE(b.recall(0).value().speed, 0.5);
    QCOMPARE(b.recall(1).value().inMs, qint64(5000));
}

void TestReplayPlaylist::fromJsonRejectsMalformed() {
    ReplayPlaylist b;
    QJsonObject bad; // no "entries" array
    QVERIFY(!b.fromJson(bad));
    QCOMPARE(b.count(), 0);
}

QTEST_MAIN(TestReplayPlaylist)
#include "tst_replayplaylist.moc"
