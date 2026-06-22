#include <QtQuickTest>
#include <QQuickStyle>

class RundownRailDragSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(rundownrail_drag, RundownRailDragSetup)
#include "tst_rundownrail_drag_main.moc"
