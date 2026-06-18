#include <QtTest>

#include "recorder_engine/ingest/nativesrtaddress.h"
#include "recorder_engine/ingest/nativesrtconnectdiagnostics.h"
#include "recorder_engine/ingest/nativesrturloptions.h"

#include <QElapsedTimer>
#include <QThread>

class TestNativeSrtAddress : public QObject {
    Q_OBJECT
private slots:
    void acceptsNumericIpv4();
    void rejectsNonIpv4Hosts();
    void fillsIpv4Sockaddr();
    void extractsStreamIdOption_data();
    void extractsStreamIdOption();
    void extractsPassphraseAndKeyLength();
    void defaultsPbKeyLenWhenOnlyPassphraseGiven();
    void formatsApplicationDefinedRejectReasonWithCode();
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

void TestNativeSrtAddress::extractsPassphraseAndKeyLength() {
    const NativeSrtUrlOptions options = nativeSrtUrlOptionsFromUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?passphrase=secretpass123&pbkeylen=32")));
    QCOMPARE(options.passphrase, QStringLiteral("secretpass123"));
    QCOMPARE(options.pbKeyLen, 32);
}

void TestNativeSrtAddress::defaultsPbKeyLenWhenOnlyPassphraseGiven() {
    // Per SRT semantics, a passphrase with no explicit pbkeylen defaults to AES-128
    // (16-byte key). The streamid coexists in the same query and stays parsed.
    const NativeSrtUrlOptions options = nativeSrtUrlOptionsFromUrl(
        QUrl(QStringLiteral("srt://127.0.0.1:9000?streamid=feed&passphrase=anothersecret")));
    QCOMPARE(options.streamId, QStringLiteral("feed"));
    QCOMPARE(options.passphrase, QStringLiteral("anothersecret"));
    QCOMPARE(options.pbKeyLen, 16);
}

void TestNativeSrtAddress::formatsApplicationDefinedRejectReasonWithCode() {
    const QString message = nativeSrtConnectFailureMessage(
        0, QStringLiteral("Success"), 1404, QStringLiteral("Application-defined rejection reason"));

    QVERIFY(message.contains(QStringLiteral("Application-defined rejection reason")));
    QVERIFY(message.contains(QStringLiteral("1404")));
    QVERIFY(!message.contains(QStringLiteral("Success")));
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
