#ifndef NDIRUNTIMEPATHS_H
#define NDIRUNTIMEPATHS_H

#include <QDir>
#include <QStringList>

namespace olr::ndi {

inline void appendRuntimeCandidate(QStringList& candidates, const QString& candidate) {
    const QString trimmed = candidate.trimmed();
    if (!trimmed.isEmpty() && !candidates.contains(trimmed)) candidates.append(trimmed);
}

inline QStringList runtimeLibraryCandidates() {
    QStringList candidates;
    const QByteArray explicitPath = qgetenv("OLR_NDI_RUNTIME_LIBRARY");
    if (!explicitPath.isEmpty())
        appendRuntimeCandidate(candidates, QString::fromLocal8Bit(explicitPath));

#if defined(Q_OS_WIN)
    const QString runtimeDir = QString::fromLocal8Bit(qgetenv("NDI_RUNTIME_DIR_V6"));
    if (!runtimeDir.isEmpty())
        appendRuntimeCandidate(
            candidates, QDir(runtimeDir).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll")));
    const QString runtimeDirV5 = QString::fromLocal8Bit(qgetenv("NDI_RUNTIME_DIR_V5"));
    if (!runtimeDirV5.isEmpty())
        appendRuntimeCandidate(
            candidates, QDir(runtimeDirV5).filePath(QStringLiteral("Processing.NDI.Lib.x64.dll")));
    const QString programFiles = QString::fromLocal8Bit(qgetenv("ProgramFiles"));
    if (!programFiles.isEmpty())
        appendRuntimeCandidate(
            candidates,
            QDir(programFiles)
                .filePath(QStringLiteral("NDI/NDI 6 Runtime/v6/Processing.NDI.Lib.x64.dll")));
    appendRuntimeCandidate(candidates, QStringLiteral("Processing.NDI.Lib.x64.dll"));
#elif defined(Q_OS_MACOS)
    appendRuntimeCandidate(candidates,
                           QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib"));
    appendRuntimeCandidate(
        candidates,
        QStringLiteral("/Applications/NDI Scan Converter.app/Contents/Frameworks/libndi.dylib"));

    const QDir applications(QStringLiteral("/Applications"));
    const QStringList ndiApps = applications.entryList({QStringLiteral("NDI*.app")}, QDir::Dirs);
    for (const QString& appName : ndiApps) {
        const QDir frameworks(
            applications.filePath(appName + QStringLiteral("/Contents/Frameworks")));
        appendRuntimeCandidate(candidates, frameworks.filePath(QStringLiteral("libndi.dylib")));
        appendRuntimeCandidate(candidates,
                               frameworks.filePath(QStringLiteral("libndi_advanced.dylib")));
    }

    appendRuntimeCandidate(candidates, QStringLiteral("/usr/local/lib/libndi.dylib"));
    appendRuntimeCandidate(candidates, QStringLiteral("libndi.dylib"));
    appendRuntimeCandidate(candidates, QStringLiteral("libndi_advanced.dylib"));
#else
    appendRuntimeCandidate(candidates, QStringLiteral("libndi.so"));
    appendRuntimeCandidate(candidates, QStringLiteral("libndi_advanced.so"));
#endif
    return candidates;
}

} // namespace olr::ndi

#endif // NDIRUNTIMEPATHS_H
