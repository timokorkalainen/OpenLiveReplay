#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "recorder_engine/replaymanager.h"
#include "uimanager.h"
#include "playback/frameprovider.h"

#include <QString>
using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    ReplayManager replayManager;
    UIManager uiManager(&replayManager);

    /*QDirIterator it(":", QDirIterator::Subdirectories);
    while (it.hasNext()) {
        qDebug() << it.next();
    }*/

    uiManager.loadSettings();

    qmlRegisterType<FrameProvider>("Recorder.Types", 1, 0, "FrameProvider");
    qmlRegisterType<PlaybackTransport>("Recorder.Types", 1, 0, "PlaybackTransport");

    QQmlApplicationEngine qmlEngine;

    // This makes the 'uiManager' object globally available in QML
    qmlEngine.rootContext()->setContextProperty("uiManager", &uiManager);

    QObject::connect(
        &qmlEngine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    //qmlEngine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    //qmlEngine.load(QUrl(u":/qt/qml/OpenLiveReplay/Main.qml"_s));
    qmlEngine.load(QUrl(u"qrc:/qt/qml/OpenLiveReplay/Main.qml"_s));

    return app.exec();
}
