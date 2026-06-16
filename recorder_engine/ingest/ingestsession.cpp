#include "ingestsession.h"

#include <QUrlQuery>

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

QUrl augmentSrtUrl(const QUrl& url) {
    if (url.scheme().toLower() != QStringLiteral("srt")) {
        return url;
    }
    QUrl out = url;
    QUrlQuery query(out);
    const auto addIfAbsent = [&query](const QString& key, const QString& value) {
        if (!query.hasQueryItem(key)) query.addQueryItem(key, value);
    };
    // ffmpeg latency options are microseconds (-> /1000 -> SRTO_LATENCY ms).
    const QString latencyUs = QString::number(qint64(kSrtLatencyMs) * 1000);
    addIfAbsent(QStringLiteral("latency"), latencyUs);
    addIfAbsent(QStringLiteral("rcvlatency"), latencyUs);
    addIfAbsent(QStringLiteral("peerlatency"), latencyUs);
    addIfAbsent(QStringLiteral("transtype"), QStringLiteral("live"));
    addIfAbsent(QStringLiteral("connect_timeout"), QString::number(kSrtConnectTimeoutMs));
    addIfAbsent(QStringLiteral("linger"), QStringLiteral("0"));
    out.setQuery(query);
    return out;
}

int jitterWindowMs(const QString& scheme, int srtFloorMs, int defaultMs) {
    return scheme.toLower() == QStringLiteral("srt") ? srtFloorMs : defaultMs;
}
