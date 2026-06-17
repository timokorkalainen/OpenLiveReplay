#include "nativesrtconnectdiagnostics.h"

namespace {
constexpr int kSrtSuccessCode = 0;
constexpr int kSrtRejectUnknown = 0;

bool hasUsefulLastError(int srtErrorCode, const QString& lastError) {
    return srtErrorCode != kSrtSuccessCode && !lastError.isEmpty() &&
           lastError != QStringLiteral("Success");
}
} // namespace

QString nativeSrtConnectFailureMessage(int srtErrorCode, const QString& lastError,
                                       int rejectReason, const QString& rejectReasonText) {
    if (rejectReason != kSrtRejectUnknown) {
        const QString rejectSummary = rejectReasonText.isEmpty()
            ? QStringLiteral("reason %1").arg(rejectReason)
            : QStringLiteral("%1, reason %2").arg(rejectReasonText).arg(rejectReason);
        if (hasUsefulLastError(srtErrorCode, lastError)) {
            return QStringLiteral("%1 (reject reason: %2)").arg(lastError, rejectSummary);
        }
        return QStringLiteral("peer rejected connection (%1)").arg(rejectSummary);
    }
    return lastError.isEmpty() ? QStringLiteral("unknown error") : lastError;
}
