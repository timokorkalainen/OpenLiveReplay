#include <QtQuickTest>
#include <QQuickStyle>

class StatusStripOnAirSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(statusstrip_onair, StatusStripOnAirSetup)
#include "tst_statusstrip_onair_main.moc"
