#include <QtTest/QtTest>

#include <QFile>

class TestMainQmlWiring : public QObject {
    Q_OBJECT

private slots:
    void fullscreenMultiviewButtonOpensWindowDirectly();
};

static QString fullscreenHandlerBody(const QString& qml) {
    const int handler = qml.indexOf(QStringLiteral("onFullscreenMultiviewRequested"));
    if (handler < 0) return {};
    const int openBrace = qml.indexOf(QLatin1Char('{'), handler);
    if (openBrace < 0) return {};

    int depth = 0;
    for (int i = openBrace; i < qml.size(); ++i) {
        if (qml.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (qml.at(i) == QLatin1Char('}')) {
            --depth;
            if (depth == 0) return qml.mid(openBrace + 1, i - openBrace - 1);
        }
    }
    return {};
}

void TestMainQmlWiring::fullscreenMultiviewButtonOpensWindowDirectly() {
    QFile file(QStringLiteral(OLR_SOURCE_DIR "/Main.qml"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.errorString()));

    const QString qml = QString::fromUtf8(file.readAll());
    const QString body = fullscreenHandlerBody(qml);
    QVERIFY2(!body.isEmpty(), "StatusStrip fullscreen handler not found in Main.qml");
    QVERIFY2(
        body.contains(QStringLiteral("openMultiviewOnExternalDisplay()")),
        qPrintable(
            QStringLiteral("Handler does not open fullscreen multiview directly:\n%1").arg(body)));
    QVERIFY2(!body.contains(QStringLiteral("screenMenu.open()")),
             "Fullscreen button should not stop at the screen-selection menu");
}

QTEST_MAIN(TestMainQmlWiring)
#include "tst_mainqml_wiring.moc"
