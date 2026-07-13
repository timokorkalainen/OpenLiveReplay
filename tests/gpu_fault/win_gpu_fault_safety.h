#ifndef OLR_WIN_GPU_FAULT_SAFETY_H
#define OLR_WIN_GPU_FAULT_SAFETY_H

#include <QJsonObject>
#include <QString>

#include <windows.h>

namespace winGpuFault {

struct TdrPolicySnapshot {
    DWORD level = 3;
    DWORD delaySeconds = 2;
    DWORD ddiDelaySeconds = 5;
    DWORD debugMode = 2;
    DWORD limitCount = 5;
    DWORD limitTimeSeconds = 60;
    bool testModePresent = false;
};

inline bool validateTdrPolicy(const TdrPolicySnapshot& policy, QString* reason) {
    auto reject = [&](const QString& value) {
        if (reason) *reason = value;
        return false;
    };
    if (policy.level != 3)
        return reject(QStringLiteral("TdrLevel must be Recover (3), got %1").arg(policy.level));
    if (policy.delaySeconds < 1 || policy.delaySeconds > 10)
        return reject(
            QStringLiteral("TdrDelay must be 1..10 seconds, got %1").arg(policy.delaySeconds));
    if (policy.ddiDelaySeconds < 1 || policy.ddiDelaySeconds > 10)
        return reject(QStringLiteral("TdrDdiDelay must be 1..10 seconds, got %1")
                          .arg(policy.ddiDelaySeconds));
    if (policy.debugMode != 2 && policy.debugMode != 3)
        return reject(
            QStringLiteral("TdrDebugMode must recover without a debugger (2 or 3), got %1")
                .arg(policy.debugMode));
    if (policy.limitCount < 5)
        return reject(
            QStringLiteral("TdrLimitCount must be at least 5, got %1").arg(policy.limitCount));
    if (policy.limitTimeSeconds < 1 || policy.limitTimeSeconds > 60)
        return reject(QStringLiteral("TdrLimitTime must be 1..60 seconds, got %1")
                          .arg(policy.limitTimeSeconds));
    if (policy.testModePresent) return reject(QStringLiteral("reserved TdrTestMode is present"));
    if (reason) reason->clear();
    return true;
}

inline bool readTdrPolicy(TdrPolicySnapshot* policy, QString* error) {
    if (!policy) return false;
    constexpr wchar_t kPath[] = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
    auto read = [&](const wchar_t* name, DWORD defaultValue, DWORD* value,
                    bool* present = nullptr) {
        DWORD type = 0;
        DWORD size = sizeof(*value);
        *value = defaultValue;
        const LSTATUS status =
            RegGetValueW(HKEY_LOCAL_MACHINE, kPath, name, RRF_RT_REG_DWORD, &type, value, &size);
        if (status == ERROR_FILE_NOT_FOUND) {
            if (present) *present = false;
            *value = defaultValue;
            return true;
        }
        if (status != ERROR_SUCCESS) {
            if (error)
                *error = QStringLiteral("cannot read GraphicsDrivers/%1 (Win32 %2)")
                             .arg(QString::fromWCharArray(name))
                             .arg(status);
            return false;
        }
        if (present) *present = true;
        return true;
    };

    DWORD testMode = 0;
    return read(L"TdrLevel", 3, &policy->level) && read(L"TdrDelay", 2, &policy->delaySeconds) &&
           read(L"TdrDdiDelay", 5, &policy->ddiDelaySeconds) &&
           read(L"TdrDebugMode", 2, &policy->debugMode) &&
           read(L"TdrLimitCount", 5, &policy->limitCount) &&
           read(L"TdrLimitTime", 60, &policy->limitTimeSeconds) &&
           read(L"TdrTestMode", 0, &testMode, &policy->testModePresent);
}

inline QJsonObject tdrPolicyEvidence(const TdrPolicySnapshot& policy) {
    return {{QStringLiteral("tdrLevel"), int(policy.level)},
            {QStringLiteral("tdrDelaySeconds"), int(policy.delaySeconds)},
            {QStringLiteral("tdrDdiDelaySeconds"), int(policy.ddiDelaySeconds)},
            {QStringLiteral("tdrDebugMode"), int(policy.debugMode)},
            {QStringLiteral("tdrLimitCount"), int(policy.limitCount)},
            {QStringLiteral("tdrLimitTimeSeconds"), int(policy.limitTimeSeconds)},
            {QStringLiteral("tdrTestModePresent"), policy.testModePresent}};
}

inline bool readAndValidateTdrPolicy(TdrPolicySnapshot* policy, QString* reason) {
    return readTdrPolicy(policy, reason) && validateTdrPolicy(*policy, reason);
}

} // namespace winGpuFault

#endif // OLR_WIN_GPU_FAULT_SAFETY_H
