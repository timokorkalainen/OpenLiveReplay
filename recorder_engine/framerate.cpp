#include "framerate.h"

#include <QStringList>

#include <cmath>

const std::vector<FrameRatePreset>& frameRatePresets() {
    static const std::vector<FrameRatePreset> kPresets = {
        {"23.976", {24000, 1001}}, {"24", {24, 1}}, {"25", {25, 1}},
        {"29.97", {30000, 1001}},  {"30", {30, 1}}, {"50", {50, 1}},
        {"59.94", {60000, 1001}},  {"60", {60, 1}},
    };
    return kPresets;
}

FrameRate parseFrameRate(const QString& s) {
    const QString t = s.trimmed();
    if (t.contains('/')) {
        const QStringList parts = t.split('/');
        if (parts.size() == 2) {
            bool okN = false, okD = false;
            const int n = parts[0].trimmed().toInt(&okN);
            const int d = parts[1].trimmed().toInt(&okD);
            if (okN && okD && n > 0 && d > 0) return FrameRate{n, d};
        }
        return FrameRate{30, 1};
    }
    for (const FrameRatePreset& p : frameRatePresets()) {
        if (t == QString::fromLatin1(p.label)) return p.rate;
    }
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || v <= 0) return FrameRate{30, 1};
    for (const FrameRatePreset& p : frameRatePresets()) {
        if (std::fabs(p.rate.toDouble() - v) < 0.05) return p.rate; // snap typed decimals
    }
    return FrameRate{int(std::lround(v)), 1};
}

QString frameRateLabel(const FrameRate& r) {
    for (const FrameRatePreset& p : frameRatePresets()) {
        if (p.rate == r) return QString::fromLatin1(p.label);
    }
    return QStringLiteral("%1/%2").arg(r.num).arg(r.den);
}
