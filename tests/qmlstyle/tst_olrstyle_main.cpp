#include <QtQuickTest>
#include <QQmlEngine>
#include <QQuickStyle>

// Select the bespoke style before the QML engine loads, exactly as the app does.
class OlrStyleSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(olrstyle, OlrStyleSetup)
#include "tst_olrstyle_main.moc"
