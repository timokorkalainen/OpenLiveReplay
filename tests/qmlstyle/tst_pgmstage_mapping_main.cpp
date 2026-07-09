#include <QtQuickTest>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQmlEngine>

class FramePreviewItemStub : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariant provider READ provider WRITE setProvider NOTIFY providerChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
public:
    QVariant provider() const { return m_provider; }
    void setProvider(const QVariant& provider) {
        if (m_provider == provider) return;
        m_provider = provider;
        emit providerChanged();
    }

    bool active() const { return m_active; }
    void setActive(bool active) {
        if (m_active == active) return;
        m_active = active;
        emit activeChanged();
    }

signals:
    void providerChanged();
    void activeChanged();

private:
    QVariant m_provider;
    bool m_active = false;
};

class PgmStageMappingSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        qmlRegisterType<FramePreviewItemStub>("Recorder.Types", 1, 0, "FramePreviewItem");
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(pgmstage_mapping, PgmStageMappingSetup)
#include "tst_pgmstage_mapping_main.moc"
