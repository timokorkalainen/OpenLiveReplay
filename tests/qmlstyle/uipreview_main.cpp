// Dev-only headless preview: render a QML file under the bespoke OlrStyle to a PNG.
// Links the OlrTheme/OlrStyle modules (no plugin dlopen, so no codesign/Team-ID issue
// the `qml` tool hits). Run with QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software.
//   uipreview <input.qml> <output.png> [width] [height]
#include <QGuiApplication>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickView>
#include <QUrl>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        qWarning("usage: uipreview <input.qml> <output.png> [width] [height]");
        return 2;
    }
    QQuickStyle::setStyle(QStringLiteral("OlrStyle"));
    QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    if (argc >= 5)
        view.resize(QString(argv[3]).toInt(), QString(argv[4]).toInt());
    else
        view.resize(1120, 640);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() != QQuickView::Ready) {
        for (const QQmlError& e : view.errors())
            qWarning() << e.toString();
        return 1;
    }
    view.show();
    app.processEvents();
    const QImage img = view.grabWindow();
    if (img.isNull()) {
        qWarning("grabWindow() returned a null image");
        return 1;
    }
    return img.save(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
}
