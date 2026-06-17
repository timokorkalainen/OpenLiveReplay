#include <QtTest>

#include "recorder_engine/ingest/nativesrtaddress.h"
#include "recorder_engine/ingest/nativesrtconnectdiagnostics.h"
#include "recorder_engine/ingest/nativesrturloptions.h"

class TestNativeSrtAddress : public QObject {
    Q_OBJECT
private slots:
    void acceptsNumericIpv4();
    void rejectsNonIpv4Hosts();
    void fillsIpv4Sockaddr();
    void extractsStreamIdOption_data();
    void extractsStreamIdOption();
    void formatsApplicationDefinedRejectReasonWithCode();
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

void TestNativeSrtAddress::extractsStreamIdOption_data() {
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("expectedStreamId");

    QTest::newRow("plain") << QStringLiteral("srt://127.0.0.1:9000?streamid=feed")
                            << QStringLiteral("feed");
    QTest::newRow("escaped-hash-bang")
        << QStringLiteral("srt://127.0.0.1:9000?streamid=%23!::r=feed,m=request")
        << QStringLiteral("#!::r=feed,m=request");
    QTest::newRow("raw-hash-bang")
        << QStringLiteral("srt://127.0.0.1:9000?streamid=#!::r=feed,m=request")
        << QStringLiteral("#!::r=feed,m=request");
    QTest::newRow("raw-hash-bang-before-next-option")
        << QStringLiteral("srt://127.0.0.1:9000?streamid=#!::r=feed,m=request&transtype=live")
        << QStringLiteral("#!::r=feed,m=request");
    QTest::newRow("encoded-ampersand")
        << QStringLiteral("srt://127.0.0.1:9000?streamid=feed%26variant")
        << QStringLiteral("feed&variant");
}

void TestNativeSrtAddress::extractsStreamIdOption() {
    QFETCH(QString, url);
    QFETCH(QString, expectedStreamId);

    const NativeSrtUrlOptions options = nativeSrtUrlOptionsFromUrl(QUrl(url));

    QCOMPARE(options.streamId, expectedStreamId);
}

void TestNativeSrtAddress::formatsApplicationDefinedRejectReasonWithCode() {
    const QString message = nativeSrtConnectFailureMessage(
        0, QStringLiteral("Success"), 1404,
        QStringLiteral("Application-defined rejection reason"));

    QVERIFY(message.contains(QStringLiteral("Application-defined rejection reason")));
    QVERIFY(message.contains(QStringLiteral("1404")));
    QVERIFY(!message.contains(QStringLiteral("Success")));
}

QTEST_GUILESS_MAIN(TestNativeSrtAddress)
#include "tst_nativesrtaddress.moc"
