#include "playback/replayplaylist.h"

#include <QJsonArray>

int ReplayPlaylist::markIn(const QString& clipPath, qint64 inMs) {
    ReplayEntry e;
    e.clipPath = clipPath;
    e.inMs = inMs;
    e.outMs = -1;
    e.speed = 1.0;
    m_entries.append(e);
    return m_entries.size() - 1;
}

bool ReplayPlaylist::markOut(qint64 outMs) {
    if (m_entries.isEmpty()) {
        return false;
    }
    ReplayEntry& last = m_entries.last();
    if (last.outMs != -1 || outMs < last.inMs) {
        return false;
    }
    last.outMs = outMs;
    return true;
}

std::optional<ReplayEntry> ReplayPlaylist::recall(int index) const {
    if (index < 0 || index >= m_entries.size()) {
        return std::nullopt;
    }
    return m_entries.at(index);
}

void ReplayPlaylist::setSpeed(int index, double speed) {
    if (index < 0 || index >= m_entries.size()) {
        return;
    }
    m_entries[index].speed = (speed < kMinSpeed) ? kMinSpeed : speed;
}

QJsonObject ReplayPlaylist::toJson() const {
    QJsonArray arr;
    for (const ReplayEntry& e : m_entries) {
        QJsonObject o;
        o["clipPath"] = e.clipPath;
        o["inMs"] = static_cast<double>(e.inMs);
        o["outMs"] = static_cast<double>(e.outMs);
        o["speed"] = e.speed;
        arr.append(o);
    }
    QJsonObject root;
    root["entries"] = arr;
    return root;
}

bool ReplayPlaylist::fromJson(const QJsonObject& obj) {
    if (!obj.contains("entries") || !obj["entries"].isArray()) {
        return false;
    }
    QVector<ReplayEntry> parsed;
    const QJsonArray arr = obj["entries"].toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        ReplayEntry e;
        e.clipPath = o.value("clipPath").toString();
        e.inMs = static_cast<qint64>(o.value("inMs").toDouble(0.0));
        e.outMs = static_cast<qint64>(o.value("outMs").toDouble(-1.0));
        e.speed = o.value("speed").toDouble(1.0);
        parsed.append(e);
    }
    m_entries = parsed;
    return true;
}
