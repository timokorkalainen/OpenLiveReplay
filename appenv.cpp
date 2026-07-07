#include "appenv.h"

#include <QByteArray>
#include <QDir>
#include <QStandardPaths>

namespace {

QString cleanRelativePath(QString relativePath) {
    relativePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (relativePath.startsWith(QLatin1Char('/'))) {
        relativePath.remove(0, 1);
    }
    return QDir::cleanPath(relativePath);
}

} // namespace

namespace appenv {

quint16 controlPort() {
    bool ok = false;
    const int value = qgetenv("OLR_CONTROL_PORT").trimmed().toInt(&ok);
    if (ok && value >= 1 && value <= 65535) return static_cast<quint16>(value);
    return 8115;
}

QString documentsRoot() {
    const QString overrideRoot = QString::fromUtf8(qgetenv("OLR_DOCUMENTS_ROOT")).trimmed();
    if (!overrideRoot.isEmpty()) return QDir::cleanPath(overrideRoot);
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString documentsPath(const QString& relativePath) {
    QDir root(documentsRoot());
    return QDir::cleanPath(root.filePath(cleanRelativePath(relativePath)));
}

} // namespace appenv
