#include "telemetry/sseparser.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

void SseParser::reset() {
    m_buffer.clear();
    m_lastError.clear();
}

QList<TelemetryEvent> SseParser::push(const QByteArray &chunk) {
    m_buffer.append(chunk);
    return parseBufferedEvents();
}

QList<TelemetryEvent> SseParser::parseBufferedEvents() {
    QList<TelemetryEvent> events;
    for (;;) {
        int sep = m_buffer.indexOf("\n\n");
        int sepLen = 2;
        const int crlfSep = m_buffer.indexOf("\r\n\r\n");
        if (crlfSep >= 0 && (sep < 0 || crlfSep < sep)) {
            sep = crlfSep;
            sepLen = 4;
        }
        if (sep < 0) break;

        const QByteArray block = m_buffer.left(sep);
        m_buffer.remove(0, sep + sepLen);
        parseEventBlock(block, &events);
    }
    return events;
}

void SseParser::parseEventBlock(const QByteArray &block, QList<TelemetryEvent> *events) {
    QByteArray data;
    QString eventType;
    QString lastEventId;

    const QList<QByteArray> lines = block.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty() || line.startsWith(':')) continue;

        const int colon = line.indexOf(':');
        const QByteArray field = colon >= 0 ? line.left(colon) : line;
        QByteArray value = colon >= 0 ? line.mid(colon + 1) : QByteArray();
        if (value.startsWith(' ')) value.remove(0, 1);

        if (field == "event") {
            eventType = QString::fromUtf8(value);
        } else if (field == "id") {
            lastEventId = QString::fromUtf8(value);
        } else if (field == "data") {
            if (!data.isEmpty()) data.append('\n');
            data.append(value);
        }
    }

    if (data.isEmpty()) return;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        m_lastError = QStringLiteral("SSE data JSON parse error: ") + parseError.errorString();
        return;
    }

    const QJsonObject payload = doc.object();
    const QString feedId = payload.value("feedId").toString().trimmed();
    if (feedId.isEmpty()) {
        m_lastError = QStringLiteral("SSE telemetry event missing feedId");
        return;
    }

    TelemetryEvent event;
    event.feedId = feedId;
    event.eventType = eventType.isEmpty() ? QStringLiteral("message") : eventType;
    event.lastEventId = lastEventId;
    event.payload = payload;
    events->append(event);
}
