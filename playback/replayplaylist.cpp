#include "playback/replayplaylist.h"

#include <QJsonArray>
#include <QJsonValue>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool readRequiredString(const QJsonObject& obj, const char* key, QString& out) {
    const QJsonValue value = obj.value(QLatin1StringView(key));
    if (!value.isString()) {
        return false;
    }
    out = value.toString();
    return !out.isEmpty();
}

bool readRequiredInt64(const QJsonObject& obj, const char* key, qint64& out) {
    const QJsonValue value = obj.value(QLatin1StringView(key));
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<qint64>::min()) ||
        number > static_cast<double>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    out = static_cast<qint64>(number);
    return true;
}

bool readRequiredDouble(const QJsonObject& obj, const char* key, double& out) {
    const QJsonValue value = obj.value(QLatin1StringView(key));
    if (!value.isDouble()) {
        return false;
    }
    out = value.toDouble();
    return std::isfinite(out);
}

} // namespace

bool ReplayPlaylist::validRange(qint64 inMs, qint64 outMs) {
    return inMs >= 0 && (outMs < 0 || outMs >= inMs);
}

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

bool ReplayPlaylist::removeEntry(int index) {
    if (index < 0 || index >= m_entries.size()) {
        return false;
    }
    m_entries.removeAt(index);
    return true;
}

int ReplayPlaylist::insertEntry(int index, const ReplayEntry& entry) {
    if (!validRange(entry.inMs, entry.outMs)) {
        return -1;
    }
    ReplayEntry normalized = entry;
    if (normalized.speed < kMinSpeed) {
        normalized.speed = kMinSpeed;
    }
    const int target = std::clamp(index, 0, static_cast<int>(m_entries.size()));
    m_entries.insert(target, normalized);
    return target;
}

bool ReplayPlaylist::moveEntry(int fromIndex, int toIndex) {
    if (fromIndex < 0 || fromIndex >= m_entries.size() || toIndex < 0 ||
        toIndex >= m_entries.size()) {
        return false;
    }
    if (fromIndex == toIndex) {
        return true;
    }
    ReplayEntry entry = m_entries.takeAt(fromIndex);
    m_entries.insert(toIndex, entry);
    return true;
}

bool ReplayPlaylist::setEntryRange(int index, qint64 inMs, qint64 outMs) {
    if (index < 0 || index >= m_entries.size() || !validRange(inMs, outMs)) {
        return false;
    }
    m_entries[index].inMs = inMs;
    m_entries[index].outMs = outMs;
    return true;
}

void ReplayPlaylist::clear() {
    m_entries.clear();
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
        if (!v.isObject()) {
            return false;
        }
        const QJsonObject o = v.toObject();
        ReplayEntry e;
        if (!readRequiredString(o, "clipPath", e.clipPath) ||
            !readRequiredInt64(o, "inMs", e.inMs) ||
            !readRequiredInt64(o, "outMs", e.outMs) ||
            !readRequiredDouble(o, "speed", e.speed)) {
            return false;
        }
        if (!validRange(e.inMs, e.outMs)) {
            return false;
        }
        if (e.speed < kMinSpeed) {
            e.speed = kMinSpeed;
        }
        parsed.append(e);
    }
    m_entries = parsed;
    return true;
}
