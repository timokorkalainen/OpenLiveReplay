#include <QtTest>

#include "recorder_engine/ingest/nativesrtaddress.h"

class TestNativeSrtAddress : public QObject {
    Q_OBJECT
private slots:
    void acceptsNumericIpv4();
    void rejectsNonIpv4Hosts();
    void fillsIpv4Sockaddr();
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

QTEST_GUILESS_MAIN(TestNativeSrtAddress)
#include "tst_nativesrtaddress.moc"
