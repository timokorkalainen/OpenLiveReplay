#pragma once

#include <QList>
#include <QString>

struct MfH264TransformIdentity {
    QString clsid;
    QString friendlyName;
    QString moduleFileName;
    QString moduleVersion;
};

// NVIDIA's 560.94 H.264 MFT repeatedly access-violated in ntdll after otherwise
// successful activation/encode/IMFShutdown/release cycles on a GTX 1080. Keep
// this an exact tuple: later versions, other NVIDIA MFTs, and other vendors have
// not reproduced the fault and must not be inferred unsafe.
inline bool isKnownUnstableMfH264Transform(const MfH264TransformIdentity& identity) {
    return identity.clsid.compare(QStringLiteral("{60F44560-5A20-4857-BFEF-D29773CB8040}"),
                                  Qt::CaseInsensitive) == 0 &&
           identity.moduleFileName.compare(QStringLiteral("nvEncMFTH264x.dll"),
                                           Qt::CaseInsensitive) == 0 &&
           identity.moduleVersion == QStringLiteral("32.0.15.6094");
}

inline bool allowMfH264Transform(const MfH264TransformIdentity& identity, bool allowKnownUnstable) {
    return allowKnownUnstable || !isKnownUnstableMfH264Transform(identity);
}

inline int selectMfH264TransformCandidate(const QList<MfH264TransformIdentity>& identities,
                                          bool allowKnownUnstable) {
    for (qsizetype i = 0; i < identities.size(); ++i) {
        if (allowMfH264Transform(identities[i], allowKnownUnstable)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
