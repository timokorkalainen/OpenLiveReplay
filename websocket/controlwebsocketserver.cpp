#include "controlwebsocketserver.h"

#include "controlapiadapter.h"
#include "controlprotocol.h"
#include "controlstate.h"

#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

ControlWebSocketServer::ControlWebSocketServer(ControlApiAdapter *adapter, QObject *parent)
    : QObject(parent),
      m_adapter(adapter),
      m_server(new QWebSocketServer(QStringLiteral("OpenLiveReplay Control API"), QWebSocketServer::NonSecureMode, this)) {
    connect(m_server, &QWebSocketServer::newConnection, this, &ControlWebSocketServer::handleNewConnection);

    m_timecodeTimer.setSingleShot(true);
    connect(&m_timecodeTimer, &QTimer::timeout, this, &ControlWebSocketServer::publishTimecodeNow);
}

ControlWebSocketServer::~ControlWebSocketServer() {
    if (m_server) {
        m_server->close();
    }
}

bool ControlWebSocketServer::listen(const QHostAddress &address, quint16 port) {
    if (!m_adapter) {
        m_lastError = QStringLiteral("No adapter configured");
        return false;
    }

    if (!m_server->listen(address, port)) {
        m_lastError = m_server->errorString();
        return false;
    }

    m_lastError.clear();
    return true;
}

quint16 ControlWebSocketServer::serverPort() const {
    return m_server->serverPort();
}

QString ControlWebSocketServer::lastError() const {
    return m_lastError;
}

void ControlWebSocketServer::publishPatch(const QString &path, const QJsonObject &value) {
    QJsonObject messageValue = value;

    if (path == QStringLiteral("recording")) {
        messageValue = ControlState::recordingObject(*m_adapter);
    } else if (path == QStringLiteral("transport")) {
        messageValue = ControlState::transportObject(*m_adapter);
    } else if (path == QStringLiteral("settings")) {
        messageValue = ControlState::settingsObject(*m_adapter);
    } else if (path == QStringLiteral("telemetry")) {
        messageValue = ControlState::telemetryObject(*m_adapter);
    } else {
        return;
    }

    broadcastJson(ControlState::patchMessage(path, messageValue));
}

void ControlWebSocketServer::publishEvent(const QString &name, const QJsonObject &data) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("event"));
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("data"), data);

    broadcastJson(obj);
}

void ControlWebSocketServer::publishTimecodeNow() {
    m_timecodeTimer.stop();
    broadcastJson(ControlState::timecodeMessage(*m_adapter));
}

void ControlWebSocketServer::scheduleTimecode() {
    if (!m_timecodeTimer.isActive()) {
        m_timecodeTimer.start(100);
    }
}

void ControlWebSocketServer::handleNewConnection() {
    QWebSocket *socket = m_server->nextPendingConnection();
    if (!socket) {
        return;
    }

    m_sockets.insert(socket);

    connect(socket, &QWebSocket::textMessageReceived, this, &ControlWebSocketServer::handleTextMessage);
    connect(socket, &QWebSocket::binaryMessageReceived, this, &ControlWebSocketServer::handleBinaryMessage);
    connect(socket, &QWebSocket::disconnected, this, &ControlWebSocketServer::handleSocketDisconnected);

    sendJson(ControlState::snapshotMessage(*m_adapter), socket);
    sendJson(ControlState::timecodeMessage(*m_adapter), socket);
}

void ControlWebSocketServer::handleTextMessage(const QString &message) {
    auto socket = qobject_cast<QWebSocket *>(sender());
    if (!socket || !m_adapter) {
        return;
    }

    const auto parsed = ControlProtocol::parseTextMessage(message.toUtf8());
    if (!parsed.ok) {
        if (!parsed.id.isEmpty()) {
            sendJson(ControlProtocol::ackError(parsed.id, parsed.code, parsed.messageText), socket);
        } else {
            sendJson(ControlProtocol::error(parsed.code, parsed.messageText), socket);
        }
        return;
    }

    const auto validated = ControlProtocol::validateCommand(parsed.message);
    if (!validated.ok) {
        sendJson(ControlProtocol::ackError(parsed.message.id, validated.code, validated.message), socket);
        return;
    }

    const auto result = m_adapter->executeCommand(parsed.message.name, validated.normalizedArgs);
    sendJson(result.ok ? ControlProtocol::ack(parsed.message.id) :
                        ControlProtocol::ackError(parsed.message.id, result.code, result.message),
             socket);
}

void ControlWebSocketServer::handleBinaryMessage(const QByteArray &) {
    auto socket = qobject_cast<QWebSocket *>(sender());
    if (!socket) return;

    sendJson(ControlProtocol::error(QStringLiteral("unsupported_message"),
                                   QStringLiteral("Only text messages are supported")),
             socket);
}

void ControlWebSocketServer::handleSocketDisconnected() {
    auto socket = qobject_cast<QWebSocket *>(sender());
    if (!socket) return;

    m_sockets.remove(socket);
    socket->deleteLater();
}

void ControlWebSocketServer::sendJson(const QJsonObject &message, QWebSocket *socket) {
    if (!socket) return;
    socket->sendTextMessage(QString::fromUtf8(ControlProtocol::compact(message)));
}

void ControlWebSocketServer::broadcastJson(const QJsonObject &message) {
    const QByteArray payload = ControlProtocol::compact(message);
    for (QWebSocket *socket : m_sockets) {
        socket->sendTextMessage(QString::fromUtf8(payload));
    }
}
