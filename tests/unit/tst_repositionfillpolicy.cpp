#include <QtTest>

#include "playback/repositionfillpolicy.h"

class TestRepositionFillPolicy : public QObject {
    Q_OBJECT

private slots:
    void selectedFeedMustReachFillTarget();
    void multiviewUsesPrimaryReferenceFeed();
    void explicitAllFeedModeWaitsForEveryFeed();
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

QTEST_GUILESS_MAIN(TestRepositionFillPolicy)
#include "tst_repositionfillpolicy.moc"
