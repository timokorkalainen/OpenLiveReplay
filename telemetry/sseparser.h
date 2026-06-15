#ifndef SSEPARSER_H
#define SSEPARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

#include "telemetry/telemetryevent.h"

class SseParser {
public:
    QList<TelemetryEvent> push(const QByteArray &chunk);
    QString lastError() const { return m_lastError; }
    void reset();

private:
    QList<TelemetryEvent> parseBufferedEvents();
    void parseEventBlock(const QByteArray &block, QList<TelemetryEvent> *events);

    QByteArray m_buffer;
    QString m_lastError;
    QString m_lastEventId;
};

#endif // SSEPARSER_H
