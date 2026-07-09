#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

#include "websocket/pendingcommandregistry.h"

class TestPendingCommandRegistry : public QObject {
    Q_OBJECT
private slots:
    void resolveDeliversOnceWithLatency() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.registerPending(1, 42, QStringLiteral("client-7"), QStringLiteral("cmd-9"));

        QJsonObject completion{{QStringLiteral("done"), true}, {QStringLiteral("pgmPts"), 5000}};
        registry.resolve(1, 42, completion);
        QCOMPARE(done.count(), 1);
        auto args = done.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("client-7"));
        QCOMPARE(args.at(1).toString(), QStringLiteral("cmd-9"));
        const QJsonObject delivered = args.at(2).toJsonObject();
        QVERIFY(delivered.value(QStringLiteral("done")).toBool());
        QVERIFY(delivered.contains(QStringLiteral("latencyMs")));

        registry.resolve(1, 42, completion); // one-shot: second resolve is dropped
        QCOMPARE(done.count(), 0);
    }

    void unmatchedResolveIsDropped() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.resolve(1, 999, QJsonObject{{QStringLiteral("done"), true}});
        QCOMPARE(done.count(), 0);
    }

    void deadlineResolvesTimeoutAndExpires() {
        PendingCommandRegistry registry;
        registry.setDeadlineMs(50);
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        QSignalSpy expired(&registry, &PendingCommandRegistry::pendingExpired);
        registry.registerPending(1, 42, QStringLiteral("c"), QStringLiteral("i"));

        registry.checkDeadlines(registry.nowMsForTest() + 49); // not yet
        QCOMPARE(done.count(), 0);
        registry.checkDeadlines(registry.nowMsForTest() + 51);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.takeFirst().at(2).toJsonObject().value(QStringLiteral("reason")).toString(),
                 QStringLiteral("timeout"));
        QCOMPARE(expired.count(), 1);
        QCOMPARE(expired.takeFirst().at(1).toULongLong(), quint64(42));
    }

    void epochChangeSupersedesOlderEntries() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.registerPending(1, 42, QStringLiteral("c"), QStringLiteral("i"));
        registry.noteWorkerEpoch(2);
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.takeFirst().at(2).toJsonObject().value(QStringLiteral("reason")).toString(),
                 QStringLiteral("superseded"));
        registry.resolve(1, 42, QJsonObject{{QStringLiteral("done"), true}}); // stale
        QCOMPARE(done.count(), 0);
    }

    void dropClientRemovesSilently() {
        PendingCommandRegistry registry;
        QSignalSpy done(&registry, &PendingCommandRegistry::commandCompleted);
        registry.registerPending(1, 42, QStringLiteral("gone"), QStringLiteral("i"));
        registry.dropClient(QStringLiteral("gone"));
        QCOMPARE(done.count(), 0);
        registry.resolve(1, 42, QJsonObject{{QStringLiteral("done"), true}});
        QCOMPARE(done.count(), 0); // entry is gone
    }
};

QTEST_GUILESS_MAIN(TestPendingCommandRegistry)
#include "tst_pendingcommandregistry.moc"
