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
    bool removeEntry(int index);
    int insertEntry(int index, const ReplayEntry& entry);
    bool moveEntry(int fromIndex, int toIndex);
    bool setEntryRange(int index, qint64 inMs, qint64 outMs);
    int count() const { return m_entries.size(); }
    QVector<ReplayEntry> entries() const { return m_entries; } // snapshot for playout
    void clear();

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& obj);

private:
    QVector<ReplayEntry> m_entries;
    static constexpr double kMinSpeed = 0.01;
    static bool validRange(qint64 inMs, qint64 outMs);
};

#endif // REPLAYPLAYLIST_H
