#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "recorder_engine/replaymanager.h"

#include <QString>
using namespace Qt::StringLiterals;

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    ReplayManager manager;

    QQmlApplicationEngine engine;

    // This makes the 'replayManager' object globally available in QML
    engine.rootContext()->setContextProperty("replayManager", &manager);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("OpenLiveReplay", "Main");

    /*const QUrl url(u"qrc:/ReplaySystem/Main.qml"_s);
    engine.load(url);*/

    return app.exec();
}
