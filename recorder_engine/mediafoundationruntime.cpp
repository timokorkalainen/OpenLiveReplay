#include "recorder_engine/mediafoundationruntime.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <mfapi.h>

#include <mutex>

namespace {

std::once_flag g_mfStartupOnce;
HRESULT g_mfStartupResult = S_OK;

QString hrMessage(const QString& action, HRESULT hr) {
    return QStringLiteral("%1 (HRESULT 0x%2)")
        .arg(action)
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

} // namespace

bool ensureMediaFoundationRuntime(QString* error) {
    std::call_once(g_mfStartupOnce,
                   [] { g_mfStartupResult = MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    if (FAILED(g_mfStartupResult)) {
        if (error) {
            *error =
                hrMessage(QStringLiteral("Media Foundation startup failed"), g_mfStartupResult);
        }
        return false;
    }
    return true;
}

#else

bool ensureMediaFoundationRuntime(QString*) {
    return true;
}

#endif
