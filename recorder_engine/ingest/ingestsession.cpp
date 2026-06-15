#include "ingestsession.h"

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    if (options.preferNativeSrt && url.scheme().toLower() == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }

    return IngestBackendKind::Ffmpeg;
}
