#ifndef TELEMETRYTIMELINEREADER_H
#define TELEMETRYTIMELINEREADER_H

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class TelemetryTimelineReader {
public:
    bool load(const QString &filePath);
    QVariantMap stateAt(qint64 playheadMs) const;
    QString lastError() const { return m_lastError; }
    QStringList feedIds() const { return m_feedIds; }

private:
    struct Entry {
        qint64 ptsMs = 0;
        QJsonObject payload;
    };

    QString m_lastError;
    QStringList m_feedIds;
    QMap<QString, QList<Entry>> m_events;
};

#endif // TELEMETRYTIMELINEREADER_H
