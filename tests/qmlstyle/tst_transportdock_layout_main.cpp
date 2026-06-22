#include <QtQuickTest>
#include <QQuickStyle>

class TransportDockLayoutSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(transportdock_layout, TransportDockLayoutSetup)
#include "tst_transportdock_layout_main.moc"
