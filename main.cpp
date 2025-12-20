#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include "recorder_engine/replaymanager.h"
#include "uimanager.h"

#include <QString>
using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    ReplayManager replayManager;
    UIManager uiManager(&replayManager);

    uiManager.loadSettings();

    QQmlApplicationEngine qmlEngine;

    // This makes the 'uiManager' object globally available in QML
    qmlEngine.rootContext()->setContextProperty("uiManager", &uiManager);

    QObject::connect(
        &qmlEngine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    //const QUrl url(QStringLiteral("qrc:/Main.qml"));
    //qmlEngine.loadFromModule("OpenLiveReplay", "Main");
    qmlEngine.load(QUrl(QStringLiteral("qrc:/Main.qml")));

    return app.exec();
}
