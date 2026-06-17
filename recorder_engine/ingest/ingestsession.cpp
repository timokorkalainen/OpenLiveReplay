#include "ingestsession.h"

#include <QUrlQuery>
#include <QtGlobal>

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    if (options.preferNativeSrt && url.scheme().toLower() == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }
    const QString scheme = url.scheme().toLower();
    if (options.preferNativeRtmp &&
        (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps"))) {
        return IngestBackendKind::NativeRtmp;
    }

    return IngestBackendKind::Ffmpeg;
}

SourceHealth srtHealth(const IngestStats& prev, const IngestStats& cur, double amberRetransRate) {
    const qint64 dDrop = cur.dropTotal - prev.dropTotal;
    const qint64 dRetrans = cur.retransTotal - prev.retransTotal;
    const qint64 dRecv = cur.recvTotal - prev.recvTotal;
    if (dDrop > 0) {
        return SourceHealth::Red;
    }
    if (dRecv > 0 && double(dRetrans) / double(dRecv) > amberRetransRate) {
        return SourceHealth::Amber;
    }
    return SourceHealth::Green;
}

SourceHealth rtmpHealth(const IngestStats& prev, const IngestStats& cur) {
    if (cur.bytesTotal < prev.bytesTotal || cur.decodeFailures < prev.decodeFailures) {
        return SourceHealth::Green; // counters reset on reconnect
    }
    const bool bytesAdvanced = cur.bytesTotal > prev.bytesTotal;
    const bool decodeFailedThisWindow = cur.decodeFailures > prev.decodeFailures;
    if (cur.lastPacketAgeMs >= kRtmpRedStallMs) {
        return SourceHealth::Red;
    }
    if (decodeFailedThisWindow && !bytesAdvanced) {
        return SourceHealth::Red;
    }
    if (decodeFailedThisWindow || cur.lastPacketAgeMs >= kRtmpAmberStallMs ||
        cur.keyframeAgeMs >= kRtmpAmberKeyframeMs) {
        return SourceHealth::Amber;
    }
    return SourceHealth::Green;
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

bool shouldStopNativeRtmpAfterFailure(IngestFailureKind failure) {
    return failure == IngestFailureKind::UnsupportedProfile ||
           failure == IngestFailureKind::DecodeCapability ||
           failure == IngestFailureKind::MalformedStream;
}

IngestBackendOptions ingestBackendOptionsFromEnvironment(const QUrl& url, bool nativeSrtAvailable,
                                                         bool nativeRtmpAvailable) {
    IngestBackendOptions options;
    const QString scheme = url.scheme().toLower();
    options.preferNativeSrt = nativeSrtAvailable && qEnvironmentVariableIsSet("OLR_NATIVE_SRT") &&
                              scheme == QStringLiteral("srt");
    options.preferNativeRtmp = nativeRtmpAvailable && (scheme == QStringLiteral("rtmp") ||
                                                       scheme == QStringLiteral("rtmps"));
    return options;
}
