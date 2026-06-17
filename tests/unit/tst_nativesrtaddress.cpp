#include <QtTest>

#include "recorder_engine/ingest/nativesrtaddress.h"

#include <QElapsedTimer>
#include <QThread>

class TestNativeSrtAddress : public QObject {
    Q_OBJECT
private slots:
    void acceptsNumericIpv4();
    void rejectsNonIpv4Hosts();
    void fillsIpv4Sockaddr();
    void resolvesHostnames();
    void triesLaterResolvedAddressesAfterFirstFailure();
    void stopsTryingAddressesWhenStopRequested();
    void resolverTimeoutReturnsBeforeCallbackFinishes();
};

void TestNativeSrtAddress::acceptsNumericIpv4() {
    QVERIFY(nativeSrtIsNumericIpv4Host(QStringLiteral("127.0.0.1")));
    QVERIFY(nativeSrtIsNumericIpv4Host(QStringLiteral("10.20.30.40")));
}

void TestNativeSrtAddress::rejectsNonIpv4Hosts() {
    QVERIFY(!nativeSrtIsNumericIpv4Host(QString()));
    QVERIFY(!nativeSrtIsNumericIpv4Host(QStringLiteral("localhost")));
    QVERIFY(!nativeSrtIsNumericIpv4Host(QStringLiteral("::1")));
    QVERIFY(!nativeSrtIsNumericIpv4Host(QStringLiteral("999.1.1.1")));
}

void TestNativeSrtAddress::fillsIpv4Sockaddr() {
    NativeSrtSockaddr address;
    QVERIFY(nativeSrtMakeIpv4Sockaddr(QStringLiteral("127.0.0.1"), 9000, &address));
    QVERIFY(address.size > 0);
    QVERIFY(address.size <= int(sizeof(address.storage)));
    QVERIFY(address.sockaddrPtr());
}

void TestNativeSrtAddress::resolvesHostnames() {
    NativeSrtSockaddr address;
    QVERIFY(nativeSrtResolveSockaddr(QStringLiteral("localhost"), 8890, &address));
    QVERIFY(address.size > 0);
    QVERIFY(address.size <= int(sizeof(address.storage)));
    QVERIFY(address.sockaddrPtr());
}

void TestNativeSrtAddress::triesLaterResolvedAddressesAfterFirstFailure() {
    NativeSrtSockaddr first;
    NativeSrtSockaddr second;
    QVERIFY(nativeSrtMakeIpv4Sockaddr(QStringLiteral("127.0.0.1"), 8890, &first));
    QVERIFY(nativeSrtMakeIpv4Sockaddr(QStringLiteral("127.0.0.2"), 8890, &second));

    int attempts = 0;
    QString error;
    QVERIFY(nativeSrtTrySockaddrs(
        {first, second},
        [&attempts](const NativeSrtSockaddr&, QString*) {
            ++attempts;
            return attempts == 2;
        },
        &error));
    QCOMPARE(attempts, 2);
    QVERIFY(error.isEmpty());
}

void TestNativeSrtAddress::stopsTryingAddressesWhenStopRequested() {
    NativeSrtSockaddr first;
    NativeSrtSockaddr second;
    QVERIFY(nativeSrtMakeIpv4Sockaddr(QStringLiteral("127.0.0.1"), 8890, &first));
    QVERIFY(nativeSrtMakeIpv4Sockaddr(QStringLiteral("127.0.0.2"), 8890, &second));

    bool stopRequested = false;
    int attempts = 0;
    QString error;
    QVERIFY(!nativeSrtTrySockaddrs(
        {first, second},
        [&attempts, &stopRequested](const NativeSrtSockaddr&, QString* attemptError) {
            ++attempts;
            stopRequested = true;
            if (attemptError) {
                *attemptError = QStringLiteral("cancelled");
            }
            return false;
        },
        &error, [&stopRequested]() { return stopRequested; }));
    QCOMPARE(attempts, 1);
    QCOMPARE(error, QStringLiteral("cancelled"));
}

void TestNativeSrtAddress::resolverTimeoutReturnsBeforeCallbackFinishes() {
    QElapsedTimer elapsed;
    elapsed.start();
    const QList<NativeSrtSockaddr> addresses = nativeSrtResolveSockaddrsWithTimeout(
        []() {
            QThread::msleep(300);
            return QList<NativeSrtSockaddr>();
        },
        20, []() { return false; });

    QVERIFY(addresses.isEmpty());
    QVERIFY2(elapsed.elapsed() < 250, "resolver timeout blocked on the slow callback");
}

QTEST_GUILESS_MAIN(TestNativeSrtAddress)
#include "tst_nativesrtaddress.moc"
