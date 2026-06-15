#include "telemetry/telemetryclient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

TelemetryClient::TelemetryClient(QObject *parent)
    : QObject(parent) {
}

TelemetryClient::~TelemetryClient() {
    stop();
}

void TelemetryClient::start(const QUrl &url) {
    stop();

    m_lastError.clear();
    m_parser.reset();

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Cache-Control", "no-cache");

    m_reply = m_networkAccessManager.get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &TelemetryClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &TelemetryClient::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &TelemetryClient::onError);
    emit connectedChanged(true);
}

void TelemetryClient::stop() {
    QNetworkReply *reply = m_reply;
    if (!reply) return;

    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    emit connectedChanged(false);
}

bool TelemetryClient::running() const {
    return m_reply != nullptr;
}

QString TelemetryClient::lastError() const {
    return m_lastError;
}

void TelemetryClient::onReadyRead() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) return;

    const QList<TelemetryEvent> events = m_parser.push(reply->readAll());
    for (const TelemetryEvent &event : events) {
        emit telemetryEvent(event);
    }

    const QString parserError = m_parser.lastError();
    if (!parserError.isEmpty()) {
        setLastError(parserError);
    }
}

void TelemetryClient::onFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) {
        if (reply) reply->deleteLater();
        return;
    }

    m_reply = nullptr;
    reply->deleteLater();
    emit connectedChanged(false);
}

void TelemetryClient::onError() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) return;

    setLastError(reply->errorString());
}

void TelemetryClient::setLastError(const QString &message) {
    m_lastError = message;
    emit errorOccurred(message);
}
