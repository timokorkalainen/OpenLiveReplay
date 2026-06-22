#include <QtQuickTest>
#include <QQuickStyle>

class SourceListPanelLayoutSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(sourcelistpanel_layout, SourceListPanelLayoutSetup)
#include "tst_sourcelistpanel_layout_main.moc"
