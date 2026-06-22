#include <QtTest>
#include <QJsonObject>
#include <QJsonArray>
#include "playback/replayplaylist.h"
#include <cmath>
#include <limits>

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
    void setSpeedClampsNonFinite();
    void removeEntryDropsByIndex();
    void removeOutOfRangeIsNoOp();
    void clearEmpties();
    void insertEntryAtIndex();
    void insertEntryClampsPastEnd();
    void moveEntryReordersToFinalIndex();
    void moveEntryOutOfRangeIsNoOp();
    void setEntryRangeUpdatesInAndOut();
    void setEntryRangeRejectsOutBeforeIn();
    void setEntryRangeRejectsUnsafeJsonIntegers();
    void markInRejectsInvalidInPoint();
    void jsonRoundTripPreservesEntries();
    void fromJsonRejectsMalformed();
    void fromJsonRejectsInvalidEntryRanges();
    void fromJsonRejectsMissingEntryFields();
    void fromJsonRejectsWrongTypedEntryFields();
    void fromJsonRejectsOutOfRangeIntegerFields();
    void fromJsonRejectsInvalidOpenSentinel();
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

void TestReplayPlaylist::setSpeedClampsNonFinite() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    p.markOut(1000);

    p.setSpeed(0, std::numeric_limits<double>::quiet_NaN());
    QVERIFY(std::isfinite(p.recall(0).value().speed));
    QVERIFY(p.recall(0).value().speed > 0.0);

    p.setSpeed(0, std::numeric_limits<double>::infinity());
    QVERIFY(std::isfinite(p.recall(0).value().speed));
    QVERIFY(p.recall(0).value().speed > 0.0);
}

void TestReplayPlaylist::removeEntryDropsByIndex() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    p.markOut(100);
    p.markIn("/clips/b.mkv", 200);
    p.markOut(300);

    QVERIFY(p.removeEntry(0));
    QCOMPARE(p.count(), 1);
    QCOMPARE(p.entries()[0].clipPath, QString("/clips/b.mkv"));
}

void TestReplayPlaylist::removeOutOfRangeIsNoOp() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);

    QVERIFY(!p.removeEntry(5));
    QVERIFY(!p.removeEntry(-1));
    QCOMPARE(p.count(), 1);
}

void TestReplayPlaylist::clearEmpties() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    p.markIn("/clips/b.mkv", 1);

    p.clear();
    QCOMPARE(p.count(), 0);
}

void TestReplayPlaylist::insertEntryAtIndex() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    p.markOut(100);
    p.markIn("/clips/c.mkv", 400);
    p.markOut(500);
    ReplayEntry b{QStringLiteral("/clips/b.mkv"), 200, 300, 0.5};

    QCOMPARE(p.insertEntry(1, b), 1);
    QCOMPARE(p.count(), 3);
    QCOMPARE(p.entries()[1].clipPath, QString("/clips/b.mkv"));
    QCOMPARE(p.entries()[1].speed, 0.5);
}

void TestReplayPlaylist::insertEntryClampsPastEnd() {
    ReplayPlaylist p;
    ReplayEntry a{QStringLiteral("/clips/a.mkv"), 0, 100, 1.0};

    QCOMPARE(p.insertEntry(42, a), 0);
    QCOMPARE(p.count(), 1);
    QCOMPARE(p.entries()[0].clipPath, QString("/clips/a.mkv"));
}

void TestReplayPlaylist::moveEntryReordersToFinalIndex() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);
    p.markOut(100);
    p.markIn("/clips/b.mkv", 200);
    p.markOut(300);
    p.markIn("/clips/c.mkv", 400);
    p.markOut(500);

    QVERIFY(p.moveEntry(0, 2));
    QCOMPARE(p.entries()[0].clipPath, QString("/clips/b.mkv"));
    QCOMPARE(p.entries()[1].clipPath, QString("/clips/c.mkv"));
    QCOMPARE(p.entries()[2].clipPath, QString("/clips/a.mkv"));

    QVERIFY(p.moveEntry(2, 0));
    QCOMPARE(p.entries()[0].clipPath, QString("/clips/a.mkv"));
}

void TestReplayPlaylist::moveEntryOutOfRangeIsNoOp() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 0);

    QVERIFY(!p.moveEntry(-1, 0));
    QVERIFY(!p.moveEntry(0, 4));
    QCOMPARE(p.count(), 1);
    QCOMPARE(p.entries()[0].clipPath, QString("/clips/a.mkv"));
}

void TestReplayPlaylist::setEntryRangeUpdatesInAndOut() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 100);
    p.markOut(300);

    QVERIFY(p.setEntryRange(0, 120, 360));
    QCOMPARE(p.entries()[0].inMs, qint64(120));
    QCOMPARE(p.entries()[0].outMs, qint64(360));
}

void TestReplayPlaylist::setEntryRangeRejectsOutBeforeIn() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 100);
    p.markOut(300);

    QVERIFY(!p.setEntryRange(0, 400, 200));
    QCOMPARE(p.entries()[0].inMs, qint64(100));
    QCOMPARE(p.entries()[0].outMs, qint64(300));
}

void TestReplayPlaylist::setEntryRangeRejectsUnsafeJsonIntegers() {
    ReplayPlaylist p;
    p.markIn("/clips/a.mkv", 100);
    p.markOut(300);

    constexpr qint64 unsafe = 9007199254740992LL;
    QVERIFY(!p.setEntryRange(0, 100, unsafe));
    QCOMPARE(p.entries()[0].outMs, qint64(300));

    QVERIFY(!p.setEntryRange(0, unsafe, -1));
    QCOMPARE(p.entries()[0].inMs, qint64(100));
}

void TestReplayPlaylist::markInRejectsInvalidInPoint() {
    ReplayPlaylist p;

    QCOMPARE(p.markIn("/clips/a.mkv", -1), -1);
    QCOMPARE(p.count(), 0);

    QCOMPARE(p.markIn("/clips/a.mkv", 9007199254740992LL), -1);
    QCOMPARE(p.count(), 0);
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

void TestReplayPlaylist::fromJsonRejectsInvalidEntryRanges() {
    ReplayPlaylist b;
    QJsonArray entries;
    entries.append(
        QJsonObject{{"clipPath", "/clips/a.mkv"}, {"inMs", 500}, {"outMs", 100}, {"speed", 1.0}});

    QVERIFY(!b.fromJson(QJsonObject{{"entries", entries}}));
    QCOMPARE(b.count(), 0);
}

void TestReplayPlaylist::fromJsonRejectsMissingEntryFields() {
    ReplayPlaylist b;
    QJsonArray entries;
    entries.append(QJsonObject{});

    QVERIFY(!b.fromJson(QJsonObject{{"entries", entries}}));
    QCOMPARE(b.count(), 0);
}

void TestReplayPlaylist::fromJsonRejectsWrongTypedEntryFields() {
    ReplayPlaylist b;
    QJsonArray entries;
    entries.append(QJsonObject{
        {"clipPath", 42},
        {"inMs", QStringLiteral("zero")},
        {"outMs", QJsonValue::Null},
        {"speed", QStringLiteral("fast")},
    });

    QVERIFY(!b.fromJson(QJsonObject{{"entries", entries}}));
    QCOMPARE(b.count(), 0);
}

void TestReplayPlaylist::fromJsonRejectsOutOfRangeIntegerFields() {
    ReplayPlaylist b;
    QJsonArray entries;
    entries.append(QJsonObject{
        {"clipPath", "/clips/a.mkv"},
        {"inMs", std::ldexp(1.0, 63)},
        {"outMs", -1},
        {"speed", 1.0},
    });

    QVERIFY(!b.fromJson(QJsonObject{{"entries", entries}}));
    QCOMPARE(b.count(), 0);
}

void TestReplayPlaylist::fromJsonRejectsInvalidOpenSentinel() {
    ReplayPlaylist b;
    QJsonArray entries;
    entries.append(
        QJsonObject{{"clipPath", "/clips/a.mkv"}, {"inMs", 500}, {"outMs", -2}, {"speed", 1.0}});

    QVERIFY(!b.fromJson(QJsonObject{{"entries", entries}}));
    QCOMPARE(b.count(), 0);
}

QTEST_MAIN(TestReplayPlaylist)
#include "tst_replayplaylist.moc"
