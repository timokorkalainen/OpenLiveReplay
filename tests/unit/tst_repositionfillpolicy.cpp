#include <QtTest>

#include "playback/repositionfillpolicy.h"

class TestRepositionFillPolicy : public QObject {
    Q_OBJECT

private slots:
    void selectedFeedMustReachFillTarget();
    void multiviewUsesPrimaryReferenceFeed();
    void explicitAllFeedModeWaitsForEveryFeed();
    void invalidReferenceFeedFallsBackToPrimary();
    void multiviewModeClearsSelectedFeed();
};

void TestRepositionFillPolicy::selectedFeedMustReachFillTarget() {
    QVector<qint64> newest{80, 80, -1, -1};
    auto at = [&](int feed) { return newest.value(feed, qint64(-1)); };

    QVERIFY(!repositionFillReady(newest.size(), 2, false, 80, at));
    newest[2] = 120;
    QVERIFY(repositionFillReady(newest.size(), 2, false, 80, at));
}

void TestRepositionFillPolicy::multiviewUsesPrimaryReferenceFeed() {
    const QVector<qint64> newest{80, -1, -1, -1};

    QVERIFY(repositionFillReady(
        newest.size(), -1, false, 80,
        [&](int feed) { return newest.value(feed, qint64(-1)); }));
}

void TestRepositionFillPolicy::explicitAllFeedModeWaitsForEveryFeed() {
    QVector<qint64> newest{80, 80, -1, -1};
    auto at = [&](int feed) { return newest.value(feed, qint64(-1)); };

    QVERIFY(!repositionFillReady(newest.size(), -1, true, 80, at));
    newest[2] = 120;
    newest[3] = 120;
    QVERIFY(repositionFillReady(newest.size(), -1, true, 80, at));
}

void TestRepositionFillPolicy::invalidReferenceFeedFallsBackToPrimary() {
    QCOMPARE(repositionReferenceFeed(4, -1), 0);
    QCOMPARE(repositionReferenceFeed(4, 0), 0);
    QCOMPARE(repositionReferenceFeed(4, 2), 2);
    QCOMPARE(repositionReferenceFeed(4, 4), 0);
    QCOMPARE(repositionReferenceFeed(4, 999), 0);
    QCOMPARE(repositionReferenceFeed(0, 0), -1);
}

void TestRepositionFillPolicy::multiviewModeClearsSelectedFeed() {
    QCOMPARE(playbackViewReferenceFeed(false, 2), -1);
    QCOMPARE(playbackViewReferenceFeed(false, -1), -1);
    QCOMPARE(playbackViewReferenceFeed(true, 2), 2);
}

QTEST_GUILESS_MAIN(TestRepositionFillPolicy)
#include "tst_repositionfillpolicy.moc"
