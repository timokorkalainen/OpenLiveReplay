#include "ingestsession.h"

#include <QUrlQuery>
#include <QtGlobal>

namespace {
bool nativeRtmpEnabledByEnvironment() {
    if (qEnvironmentVariableIsSet("OLR_FFMPEG_RTMP")) {
        return false;
    }

    const QString value = qEnvironmentVariable("OLR_NATIVE_RTMP").trimmed().toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true") ||
           value == QStringLiteral("on") || value == QStringLiteral("yes");
}
} // namespace

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

bool shouldFallbackToFfmpegAfterNativeFailure(IngestFailureKind failure) {
    return failure == IngestFailureKind::UnsupportedProfile ||
           failure == IngestFailureKind::DecodeCapability ||
           failure == IngestFailureKind::MalformedStream;
}

bool NativeRtmpFfmpegFallbackPolicy::shouldForceFfmpeg(const QString& url) {
    if (m_url != url) {
        m_url = url;
        m_forceFfmpeg = false;
    }
    return m_forceFfmpeg;
}

bool NativeRtmpFfmpegFallbackPolicy::recordNativeFailure(const QString& url,
                                                         IngestFailureKind failure,
                                                         bool fallbackEnabled) {
    shouldForceFfmpeg(url);
    if (!fallbackEnabled || !shouldFallbackToFfmpegAfterNativeFailure(failure)) {
        return false;
    }

    m_forceFfmpeg = true;
    return true;
}

IngestBackendOptions ingestBackendOptionsFromEnvironment(const QUrl& url, bool nativeSrtAvailable,
                                                         bool nativeRtmpAvailable) {
    IngestBackendOptions options;
    const QString scheme = url.scheme().toLower();
    options.preferNativeSrt = nativeSrtAvailable &&
                              qEnvironmentVariableIsSet("OLR_NATIVE_SRT") &&
                              scheme == QStringLiteral("srt");
    options.preferNativeRtmp = nativeRtmpAvailable &&
                               (scheme == QStringLiteral("rtmp") ||
                                scheme == QStringLiteral("rtmps")) &&
                               nativeRtmpEnabledByEnvironment();
    return options;
}
