#ifndef NATIVESRTCONNECTDIAGNOSTICS_H
#define NATIVESRTCONNECTDIAGNOSTICS_H

#include <QString>

QString nativeSrtConnectFailureMessage(int srtErrorCode, const QString& lastError,
                                       int rejectReason, const QString& rejectReasonText);

#endif // NATIVESRTCONNECTDIAGNOSTICS_H
