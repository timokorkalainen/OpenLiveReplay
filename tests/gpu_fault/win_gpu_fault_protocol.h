#ifndef OLR_WIN_GPU_FAULT_PROTOCOL_H
#define OLR_WIN_GPU_FAULT_PROTOCOL_H

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace winGpuFault {

constexpr int kSkipExitCode = 77;
constexpr int kChildTimeoutMs = 45000;

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

inline bool validateFenceEvidence(const QJsonObject& value, QString* error) {
    const bool valid = value.value(QStringLiteral("mode")) == QStringLiteral("probe-fence") &&
                       value.value(QStringLiteral("signalCount")).toInt() == 1 &&
                       value.value(QStringLiteral("signalValue")).toDouble() > 0 &&
                       value.value(QStringLiteral("initialCompleted")).toDouble() <
                           value.value(QStringLiteral("signalValue")).toDouble() &&
                       value.value(QStringLiteral("pendingInitially")).toInt() > 0 &&
                       value.value(QStringLiteral("finalCompleted")).toDouble() >=
                           value.value(QStringLiteral("signalValue")).toDouble() &&
                       value.value(QStringLiteral("pendingFinally")).toInt() == 0 &&
                       value.value(QStringLiteral("waitCount")).toInt() == 1;
    if (!valid && error) *error = QStringLiteral("fence ordering evidence failed its oracle");
    return valid;
}

inline bool validateTdrEvidence(const QJsonObject& value, QString* error) {
    const bool valid = value.value(QStringLiteral("mode")) == QStringLiteral("trigger-tdr") &&
                       value.value(QStringLiteral("destructiveStarted")).toBool() &&
                       value.value(QStringLiteral("removedHresult")).toDouble() < 0 &&
                       value.value(QStringLiteral("generationAfter")).toDouble() >
                           value.value(QStringLiteral("generationBefore")).toDouble() &&
                       value.value(QStringLiteral("realLossToken")).toBool() &&
                       value.value(QStringLiteral("tokenGeneration")).toDouble() ==
                           value.value(QStringLiteral("generationAfter")).toDouble() &&
                       value.value(QStringLiteral("staleFrameRejected")).toBool() &&
                       value.value(QStringLiteral("signalCount")).toInt() == 1 &&
                       value.value(QStringLiteral("pendingInitially")).toInt() > 0 &&
                       value.value(QStringLiteral("releasedRetains")).toInt() > 0 &&
                       value.value(QStringLiteral("deadFenceWaits")).toInt() == 0 &&
                       value.value(QStringLiteral("pendingFinally")).toInt() == 0;
    if (!valid && error) *error = QStringLiteral("real-removal evidence failed its oracle");
    return valid;
}

} // namespace winGpuFault

#endif
