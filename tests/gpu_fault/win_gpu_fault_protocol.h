#ifndef OLR_WIN_GPU_FAULT_PROTOCOL_H
#define OLR_WIN_GPU_FAULT_PROTOCOL_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace winGpuFault {

constexpr int kSkipExitCode = 77;
constexpr int kChildTimeoutMs = 45000;
constexpr int kFenceCalibrationMaxGroups = 4096;
constexpr int kFenceCalibrationMaxAttempts = 8;

inline QByteArray encode(const QJsonObject& value) {
    return QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
}

inline bool decodeSingle(const QByteArray& bytes, QJsonObject* value, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes.trimmed(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("malformed child JSON: %1").arg(parseError.errorString());
        return false;
    }
    *value = document.object();
    return true;
}

inline bool decodeMerged(const QByteArray& bytes, QJsonObject* value, QString* error) {
    QJsonObject merged;
    bool decodedAny = false;
    const QList<QByteArray> lines = bytes.split('\n');
    for (const QByteArray& rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        QJsonObject object;
        if (!decodeSingle(line, &object, error)) return false;
        for (auto it = object.begin(); it != object.end(); ++it)
            merged.insert(it.key(), it.value());
        decodedAny = true;
    }
    if (!decodedAny) {
        if (error) *error = QStringLiteral("child emitted no structured JSON evidence");
        return false;
    }
    *value = merged;
    return true;
}

// Abnormal-exit decoder: retain every fully flushed checkpoint before a
// truncated trailing record. Successful children still use strict decodeMerged.
inline bool decodeAvailable(const QByteArray& bytes, QJsonObject* value, QString* error) {
    QJsonObject merged;
    bool decodedAny = false;
    const QList<QByteArray> lines = bytes.split('\n');
    for (const QByteArray& rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        QJsonObject object;
        QString lineError;
        if (!decodeSingle(line, &object, &lineError)) {
            if (decodedAny) break;
            if (error) *error = lineError;
            return false;
        }
        for (auto it = object.begin(); it != object.end(); ++it)
            merged.insert(it.key(), it.value());
        decodedAny = true;
    }
    if (!decodedAny) {
        if (error) *error = QStringLiteral("child emitted no complete JSON checkpoint");
        return false;
    }
    *value = merged;
    return true;
}

inline bool validateFenceEvidence(const QJsonObject& value, QString* error) {
    const double calibrationElapsedNs =
        value.value(QStringLiteral("calibrationElapsedNs")).toDouble();
    const double calibrationTargetNs =
        value.value(QStringLiteral("calibrationTargetNs")).toDouble();
    const int calibrationHardCeilingMs =
        value.value(QStringLiteral("calibrationHardCeilingMs")).toInt();
    const int calibrationGroups = value.value(QStringLiteral("calibrationGroups")).toInt();
    const int calibrationMaxGroups = value.value(QStringLiteral("calibrationMaxGroups")).toInt();
    const double calibrationWarmupElapsedNs =
        value.value(QStringLiteral("calibrationWarmupElapsedNs")).toDouble();
    const int calibrationWarmupHardCeilingMs =
        value.value(QStringLiteral("calibrationWarmupHardCeilingMs")).toInt();
    const bool valid =
        value.value(QStringLiteral("mode")) == QStringLiteral("probe-fence") &&
        value.value(QStringLiteral("signalCount")).toInt() == 1 &&
        value.value(QStringLiteral("signalValue")).toDouble() > 0 &&
        value.value(QStringLiteral("initialCompleted")).toDouble() <
            value.value(QStringLiteral("signalValue")).toDouble() &&
        value.value(QStringLiteral("pendingInitially")).toInt() > 0 &&
        value.value(QStringLiteral("finalCompleted")).toDouble() >=
            value.value(QStringLiteral("signalValue")).toDouble() &&
        value.value(QStringLiteral("pendingFinally")).toInt() == 0 &&
        value.value(QStringLiteral("waitCount")).toInt() == 1 &&
        value.value(QStringLiteral("waited")).toBool() &&
        value.value(QStringLiteral("calibrationAttempts")).toInt() > 0 &&
        value.value(QStringLiteral("calibrationAttempts")).toInt() <=
            kFenceCalibrationMaxAttempts &&
        calibrationGroups > 0 && calibrationMaxGroups > 0 &&
        calibrationMaxGroups == kFenceCalibrationMaxGroups &&
        calibrationGroups <= calibrationMaxGroups && calibrationTargetNs > 0 &&
        calibrationElapsedNs >= calibrationTargetNs &&
        value.value(QStringLiteral("calibrationReachedTarget")).toBool() &&
        calibrationHardCeilingMs > 0 && calibrationHardCeilingMs <= 250 &&
        calibrationTargetNs <= double(calibrationHardCeilingMs) * 500000.0 &&
        calibrationElapsedNs < double(calibrationHardCeilingMs) * 1000000.0 &&
        calibrationWarmupHardCeilingMs > 0 && calibrationWarmupHardCeilingMs <= 500 &&
        calibrationWarmupElapsedNs > 0 &&
        calibrationWarmupElapsedNs < double(calibrationWarmupHardCeilingMs) * 1000000.0;
    if (!valid && error) *error = QStringLiteral("fence ordering evidence failed its oracle");
    return valid;
}

inline bool validateTdrEvidence(const QJsonObject& value, QString* error) {
    const QJsonValue recoveryMs = value.value(QStringLiteral("workerRecoveryMs"));
    const bool valid =
        value.value(QStringLiteral("mode")) == QStringLiteral("trigger-tdr") &&
        value.value(QStringLiteral("destructiveStarted")).toBool() &&
        value.value(QStringLiteral("jobContained")).toBool() &&
        value.value(QStringLiteral("tdrPolicySafe")).toBool() &&
        value.value(QStringLiteral("dedicatedRunner")).toBool() &&
        value.value(QStringLiteral("removedHresult")).toDouble() < 0 &&
        value.value(QStringLiteral("generationAfter")).toDouble() >
            value.value(QStringLiteral("generationBefore")).toDouble() &&
        value.value(QStringLiteral("realLossToken")).toBool() &&
        value.value(QStringLiteral("tokenlessBeforeRemoval")).toBool() &&
        value.value(QStringLiteral("faultSurfaceBound")).toBool() &&
        value.value(QStringLiteral("tokenGeneration")).toDouble() ==
            value.value(QStringLiteral("generationAfter")).toDouble() &&
        value.value(QStringLiteral("staleFrameRejected")).toBool() &&
        value.value(QStringLiteral("signalCount")).toInt() == 1 &&
        value.value(QStringLiteral("pendingInitially")).toInt() > 0 &&
        value.value(QStringLiteral("abandonAttempted")).toBool() &&
        value.value(QStringLiteral("retainedSurfaceReleased")).toBool() &&
        value.value(QStringLiteral("releasedRetains")).toInt() > 0 &&
        value.value(QStringLiteral("workerAbandonedRetains")).toInt() > 0 &&
        value.value(QStringLiteral("workerRetainedSurfaceReleased")).toBool() &&
        value.value(QStringLiteral("deadFenceWaits")).toInt() == 0 &&
        value.value(QStringLiteral("pendingFinally")).toInt() == 0 &&
        value.value(QStringLiteral("workerRecoveryExercised")).toBool() &&
        value.value(QStringLiteral("workerRealLossToken")).toBool() &&
        value.value(QStringLiteral("workerGenerationAfter")).toDouble() >
            value.value(QStringLiteral("workerGenerationBefore")).toDouble() &&
        value.value(QStringLiteral("workerStaleFrameRejected")).toBool() &&
        value.value(QStringLiteral("workerProductionPollObservedRemoval")).toBool() &&
        value.value(QStringLiteral("workerStaleFrameExcludedFromSink")).toBool() &&
        value.value(QStringLiteral("workerCacheRecoveredToCpu")).toBool() &&
        value.value(QStringLiteral("workerOutputResumed")).toBool() &&
        value.value(QStringLiteral("workerRecoveryReachedSink")).toBool() &&
        value.value(QStringLiteral("workerCoherentState")).toBool() &&
        value.value(QStringLiteral("recoveryComplete")).toBool() && recoveryMs.isDouble() &&
        recoveryMs.toDouble() >= 0 && recoveryMs.toDouble() <= 10000;
    if (!valid && error) *error = QStringLiteral("real-removal evidence failed its oracle");
    return valid;
}

} // namespace winGpuFault

#endif
