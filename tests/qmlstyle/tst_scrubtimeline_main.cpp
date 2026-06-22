#include <QtQuickTest>
#include <QQuickStyle>

class ScrubTimelineSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(scrubtimeline, ScrubTimelineSetup)
#include "tst_scrubtimeline_main.moc"
