#include <QtQuickTest>
#include <QQuickStyle>

class PgmStageMappingSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(pgmstage_mapping, PgmStageMappingSetup)
#include "tst_pgmstage_mapping_main.moc"
