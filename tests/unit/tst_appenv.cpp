#include <QtTest>

#include "appenv.h"

class TestAppEnv : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void configureQtMediaBackendUsesPlatformPolicy();
    void controlPortDefaultsToProductionPort();
    void controlPortUsesValidEnvironmentOverride();
    void controlPortIgnoresInvalidEnvironmentOverride();
    void documentsPathUsesDocumentsRootOverride();
};

void TestAppEnv::cleanup() {
    qunsetenv("OLR_CONTROL_PORT");
    qunsetenv("OLR_DOCUMENTS_ROOT");
    qunsetenv("QT_MEDIA_BACKEND");
}

void TestAppEnv::configureQtMediaBackendUsesPlatformPolicy() {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    appenv::configureQtMediaBackend();

#if defined(Q_OS_WIN)
    QCOMPARE(qgetenv("QT_MEDIA_BACKEND"), QByteArray("windows"));
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    QCOMPARE(qgetenv("QT_MEDIA_BACKEND"), QByteArray("darwin"));
#else
    QVERIFY(!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND"));
#endif
}

void TestAppEnv::controlPortDefaultsToProductionPort() {
    qunsetenv("OLR_CONTROL_PORT");

    QCOMPARE(appenv::controlPort(), quint16(8115));
}

void TestAppEnv::controlPortUsesValidEnvironmentOverride() {
    qputenv("OLR_CONTROL_PORT", "19876");

    QCOMPARE(appenv::controlPort(), quint16(19876));
}

void TestAppEnv::controlPortIgnoresInvalidEnvironmentOverride() {
    qputenv("OLR_CONTROL_PORT", "0");
    QCOMPARE(appenv::controlPort(), quint16(8115));

    qputenv("OLR_CONTROL_PORT", "70000");
    QCOMPARE(appenv::controlPort(), quint16(8115));

    qputenv("OLR_CONTROL_PORT", "not-a-port");
    QCOMPARE(appenv::controlPort(), quint16(8115));
}

void TestAppEnv::documentsPathUsesDocumentsRootOverride() {
    qputenv("OLR_DOCUMENTS_ROOT", "/tmp/olr-app-env");

    QCOMPARE(appenv::documentsPath(QStringLiteral("settings/config.json")),
             QStringLiteral("/tmp/olr-app-env/settings/config.json"));
    QCOMPARE(appenv::documentsPath(QStringLiteral("/videos/capture.jpg")),
             QStringLiteral("/tmp/olr-app-env/videos/capture.jpg"));
}

QTEST_GUILESS_MAIN(TestAppEnv)
#include "tst_appenv.moc"
