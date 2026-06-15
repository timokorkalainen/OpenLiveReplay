#include "telemetry/telemetryclient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

TelemetryClient::TelemetryClient(QObject *parent)
    : QObject(parent),
      m_networkAccessManager(new QNetworkAccessManager(this)) {
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

    m_reply = m_networkAccessManager->get(request);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &TelemetryClient::onMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &TelemetryClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &TelemetryClient::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &TelemetryClient::onError);
}

void TelemetryClient::stop() {
    QNetworkReply *reply = m_reply;
    if (!reply) return;

    m_reply = nullptr;
    const bool wasConnected = m_connected;
    m_connected = false;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    if (wasConnected) emit connectedChanged(false);
}

bool TelemetryClient::running() const {
    return m_reply != nullptr;
}

QString TelemetryClient::lastError() const {
    return m_lastError;
}

void TelemetryClient::onMetaDataChanged() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) return;

    establishConnectionIfUsable(reply, false);
}

void TelemetryClient::onReadyRead() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) return;
    if (!establishConnectionIfUsable(reply, true)) return;

    const QList<TelemetryEvent> events = m_parser.push(reply->readAll());
    for (const TelemetryEvent &event : events) {
        emit telemetryEvent(event);
    }

    const QString parserError = m_parser.lastError();
    if (!parserError.isEmpty()) {
        setLastError(parserError);
    } else if (!events.isEmpty()) {
        m_lastError.clear();
    }
}

void TelemetryClient::onFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) {
        if (reply) reply->deleteLater();
        return;
    }

    if (!m_connected) {
        handleHttpErrorStatus(reply);
    }

    const bool wasConnected = m_connected;
    m_reply = nullptr;
    m_connected = false;
    reply->deleteLater();
    if (wasConnected) emit connectedChanged(false);
}

void TelemetryClient::onError() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) return;

    setLastError(reply->errorString());
}

bool TelemetryClient::establishConnectionIfUsable(QNetworkReply *reply, bool allowMissingHttpStatus) {
    if (handleHttpErrorStatus(reply)) {
        clearReply(reply);
        return false;
    }

    const QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusCode.isValid()) {
        if (!allowMissingHttpStatus) return false;
        establishConnected();
        return true;
    }

    const int status = statusCode.toInt();
    if (status >= 200 && status < 300) {
        establishConnected();
        return true;
    }

    return false;
}

bool TelemetryClient::handleHttpErrorStatus(QNetworkReply *reply) {
    const QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusCode.isValid()) return false;

    const int status = statusCode.toInt();
    if (status < 400) return false;

    const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    QString message = QStringLiteral("HTTP %1").arg(status);
    if (!reason.isEmpty()) {
        message += QStringLiteral(": ") + reason;
    }
    setLastError(message);
    return true;
}

void TelemetryClient::establishConnected() {
    if (m_connected) return;

    m_connected = true;
    emit connectedChanged(true);
}

void TelemetryClient::clearReply(QNetworkReply *reply) {
    if (reply != m_reply) return;

    const bool wasConnected = m_connected;
    m_reply = nullptr;
    m_connected = false;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    if (wasConnected) emit connectedChanged(false);
}

void TelemetryClient::setLastError(const QString &message) {
    m_lastError = message;
    emit errorOccurred(message);
}
