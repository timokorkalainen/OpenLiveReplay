#include "ingestsession.h"

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    if (options.preferNativeSrt && url.scheme().toLower() == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }

    return IngestBackendKind::Ffmpeg;
}

SrtHealth srtHealth(const SrtStats& prev, const SrtStats& cur, double amberRetransRate) {
    const qint64 dDrop = cur.dropTotal - prev.dropTotal;
    const qint64 dRetrans = cur.retransTotal - prev.retransTotal;
    const qint64 dRecv = cur.recvTotal - prev.recvTotal;
    if (dDrop > 0) {
        return SrtHealth::Red;
    }
    if (dRecv > 0 && double(dRetrans) / double(dRecv) > amberRetransRate) {
        return SrtHealth::Amber;
    }
    return SrtHealth::Green;
}
