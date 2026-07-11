#include <QtQuickTest>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQmlContext>
#include <QQmlEngine>

#include "playback/framepreviewitem.h"
#include "playback/frameprovider.h"

class PreviewUiStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(FrameProvider* multiviewPreviewProvider READ multiviewPreviewProvider NOTIFY
                   playbackProvidersChanged)
    Q_PROPERTY(
        FrameProvider* pgmPreviewProvider READ pgmPreviewProvider NOTIFY playbackProvidersChanged)
    Q_PROPERTY(QVariantList viewSlotMap READ viewSlotMap CONSTANT)
    Q_PROPERTY(int multiviewCount READ multiviewCount CONSTANT)
    Q_PROPERTY(bool playbackSingleView READ playbackSingleView CONSTANT)
    Q_PROPERTY(int playbackSelectedIndex READ playbackSelectedIndex CONSTANT)
    Q_PROPERTY(bool multiviewHasConsumer READ multiviewHasConsumer NOTIFY playbackProvidersChanged)
    Q_PROPERTY(bool pgmHasConsumer READ pgmHasConsumer NOTIFY playbackProvidersChanged)

public:
    explicit PreviewUiStub(QObject* parent = nullptr) : QObject(parent) {
        replacePreviewProviders();
    }

    FrameProvider* multiviewPreviewProvider() const { return m_multiview; }
    FrameProvider* pgmPreviewProvider() const { return m_pgm; }
    QVariantList viewSlotMap() const { return {0, 1, 2, 3}; }
    int multiviewCount() const { return 4; }
    bool playbackSingleView() const { return false; }
    int playbackSelectedIndex() const { return -1; }
    bool multiviewHasConsumer() const { return m_multiview && m_multiview->hasPreviewConsumers(); }
    bool pgmHasConsumer() const { return m_pgm && m_pgm->hasPreviewConsumers(); }

    Q_INVOKABLE void replacePreviewProviders() {
        delete m_multiview;
        delete m_pgm;
        m_multiview = new FrameProvider(this);
        m_pgm = new FrameProvider(this);
        emit playbackProvidersChanged();
    }

    Q_INVOKABLE void setPlaybackViewState(bool, int) {}

    Q_INVOKABLE QString sourceDisplayLabel(int source) const {
        return QStringLiteral("SRC%1").arg(source);
    }

signals:
    void playbackProvidersChanged();
    void streamUrlsChanged();
    void multiviewCountChanged();
    void viewSlotMapChanged();
    void feedSelectRequested(int index);
    void playbackViewStateChanged();
    void multiviewRequested();

private:
    FrameProvider* m_multiview = nullptr;
    FrameProvider* m_pgm = nullptr;
};

class PgmStageMappingSetup : public QObject {
    Q_OBJECT
public slots:
    void applicationAvailable() {
        qmlRegisterType<FramePreviewItem>("Recorder.Types", 1, 0, "FramePreviewItem");
        qmlRegisterType<FrameProvider>("Recorder.Types", 1, 0, "FrameProvider");
        QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
        QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        engine->rootContext()->setContextProperty(QStringLiteral("previewUi"), &m_previewUi);
    }

private:
    PreviewUiStub m_previewUi;
};

QUICK_TEST_MAIN_WITH_SETUP(pgmstage_mapping, PgmStageMappingSetup)
#include "tst_pgmstage_mapping_main.moc"
