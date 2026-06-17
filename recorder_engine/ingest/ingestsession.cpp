#include "ingestsession.h"

#include <QtGlobal>

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    const QString scheme = url.scheme().toLower();
    if (options.preferNativeSrt && scheme == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }
    if (options.preferNativeRtmp &&
        (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps"))) {
        return IngestBackendKind::NativeRtmp;
    }
    return IngestBackendKind::Unsupported;
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
    options.preferNativeSrt = nativeSrtAvailable && scheme == QStringLiteral("srt");
    options.preferNativeRtmp = nativeRtmpAvailable && (scheme == QStringLiteral("rtmp") ||
                                                       scheme == QStringLiteral("rtmps"));
    return options;
}
