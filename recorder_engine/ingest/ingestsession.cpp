#include "ingestsession.h"

#include "decodedframeevidencequeue.h"

#include <QtGlobal>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

DecodedVideoFrame decodedCpuVideoFrameForOutput(AVFrame* frame,
                                                const DecodedFrameEvidence* evidence) {
    DecodedVideoFrame decodedFrame;
    decodedFrame.frame = frame;
    if (evidence) {
        decodedFrame.sourcePtsMs = evidence->sourcePtsMs;
        decodedFrame.sourceTimecode100ns = evidence->sourceTimecode100ns;
        decodedFrame.timecodeEvidence = evidence->timecodeEvidence;
    } else if (frame && frame->pts != AV_NOPTS_VALUE) {
        decodedFrame.sourcePtsMs = frame->pts / 90;
    }
    return decodedFrame;
}

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    const QString scheme = url.scheme().toLower();
    if (options.preferNativeSrt && scheme == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }
    if (options.preferNativeRtmp &&
        (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps"))) {
        return IngestBackendKind::NativeRtmp;
    }
    if (options.preferNativeNdi && scheme == QStringLiteral("ndi")) {
        return IngestBackendKind::NativeNdi;
    }
    return IngestBackendKind::Unsupported;
}

SourceHealth srtHealth(const IngestStats& prev, const IngestStats& cur, double amberRetransRate) {
    if (cur.recvTotal < prev.recvTotal || cur.retransTotal < prev.retransTotal ||
        cur.lossTotal < prev.lossTotal || cur.dropTotal < prev.dropTotal ||
        cur.decodeFailures < prev.decodeFailures) {
        return SourceHealth::Green; // counters reset on reconnect
    }
    const qint64 dDrop = cur.dropTotal - prev.dropTotal;
    const qint64 dRetrans = cur.retransTotal - prev.retransTotal;
    const qint64 dRecv = cur.recvTotal - prev.recvTotal;
    const quint64 dDecodeFailures = cur.decodeFailures - prev.decodeFailures;
    if (dDrop > 0) {
        return SourceHealth::Red;
    }
    if (dDecodeFailures > 0 && dRecv <= 0) {
        return SourceHealth::Red;
    }
    if (dRecv > 0 && double(dRetrans) / double(dRecv) > amberRetransRate) {
        return SourceHealth::Amber;
    }
    if (dDecodeFailures > 0) {
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

bool keepSurfaceDecodeNeedsResetBeforeCpuFallback(bool decodedGpu, bool gpuSurfaceRejected) {
    return !decodedGpu || gpuSurfaceRejected;
}

bool ingestPrefersGpuVideoFrames(const IngestCallbacks& callbacks) {
    if (callbacks.shouldPreferGpuVideoFrames) return callbacks.shouldPreferGpuVideoFrames();
    return callbacks.preferGpuVideoFrames;
}

IngestBackendOptions ingestBackendOptionsFromEnvironment(const QUrl& url, bool nativeSrtAvailable,
                                                         bool nativeRtmpAvailable,
                                                         bool nativeNdiAvailable) {
    IngestBackendOptions options;
    const QString scheme = url.scheme().toLower();
    options.preferNativeSrt = nativeSrtAvailable && scheme == QStringLiteral("srt");
    options.preferNativeRtmp = nativeRtmpAvailable && (scheme == QStringLiteral("rtmp") ||
                                                       scheme == QStringLiteral("rtmps"));
    options.preferNativeNdi = nativeNdiAvailable && scheme == QStringLiteral("ndi");
    return options;
}
