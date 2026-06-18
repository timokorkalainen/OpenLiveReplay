#ifndef REPLAYPLAYLIST_H
#define REPLAYPLAYLIST_H

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <optional>

struct ReplayEntry {
    QString clipPath;
    qint64 inMs = 0;
    qint64 outMs = -1; // -1 = open / not yet marked out
    double speed = 1.0;
};

// Ordered EVS-style cue list. Pure model; no I/O, no threads.
class ReplayPlaylist {
public:
    int markIn(const QString& clipPath, qint64 inMs);
    bool markOut(qint64 outMs);
    std::optional<ReplayEntry> recall(int index) const;
    void setSpeed(int index, double speed);
    int count() const { return m_entries.size(); }
    void clear() { m_entries.clear(); }

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& obj);

private:
    QVector<ReplayEntry> m_entries;
    static constexpr double kMinSpeed = 0.01;
};

#endif // REPLAYPLAYLIST_H
