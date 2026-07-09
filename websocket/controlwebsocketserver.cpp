#include "controlwebsocketserver.h"

#include "controlapiadapter.h"
#include "controlprotocol.h"
#include "controlstate.h"

#include <QAbstractSocket>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

ControlWebSocketServer::ControlWebSocketServer(ControlApiAdapter* adapter, QObject* parent)
    : QObject(parent), m_adapter(adapter),
      m_server(new QWebSocketServer(QStringLiteral("OpenLiveReplay Control API"),
                                    QWebSocketServer::NonSecureMode, this)) {
    connect(m_server, &QWebSocketServer::newConnection, this,
            &ControlWebSocketServer::handleNewConnection);
    connect(m_server, &QWebSocketServer::acceptError, this,
            [this](QAbstractSocket::SocketError error) {
                qWarning() << "WebSocket control API accept error" << int(error)
                           << m_server->errorString();
            });
    connect(m_server, &QWebSocketServer::closed, this,
            []() { qInfo() << "WebSocket control API closed"; });

    if (m_adapter) {
        if (QObject* notifier = m_adapter->completionNotifier()) {
            connect(notifier, SIGNAL(commandCompleted(QString, QString, QJsonObject)), this,
                    SLOT(deliverCommandCompleted(QString, QString, QJsonObject)));
        }
    }

    m_timecodeTimer.setSingleShot(true);
    connect(&m_timecodeTimer, &QTimer::timeout, this, &ControlWebSocketServer::publishTimecodeNow);
}

ControlWebSocketServer::~ControlWebSocketServer() {
    m_timecodeTimer.stop();

    if (m_server) {
        m_server->close();
    }

    const QSet<QWebSocket*> sockets = m_sockets;
    for (QWebSocket* socket : sockets) {
        if (!socket) continue;
        socket->close();
        socket->deleteLater();
    }
    m_sockets.clear();
}

bool ControlWebSocketServer::listen(const QHostAddress& address, quint16 port) {
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

void ControlWebSocketServer::publishPatch(const QString& path, const QJsonObject& value) {
    if (!m_adapter) return;

    QJsonObject messageValue = value;

    if (path == QStringLiteral("recording")) {
        messageValue = ControlState::recordingObject(*m_adapter);
    } else if (path == QStringLiteral("transport")) {
        messageValue = ControlState::transportObject(*m_adapter);
    } else if (path == QStringLiteral("settings")) {
        messageValue = ControlState::settingsObject(*m_adapter);
    } else if (path == QStringLiteral("telemetry")) {
        messageValue = ControlState::telemetryObject(*m_adapter);
    } else if (path == QStringLiteral("output")) {
        messageValue = ControlState::outputObject(*m_adapter);
    } else {
        return;
    }

    broadcastJson(ControlState::patchMessage(path, messageValue));
}

void ControlWebSocketServer::publishPatchObject(const QString& path, const QJsonObject& value) {
    broadcastJson(ControlState::patchMessage(path, value));
}

void ControlWebSocketServer::publishEvent(const QString& name, const QJsonObject& data) {
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("event"));
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("data"), data);

    broadcastJson(obj);
}

void ControlWebSocketServer::publishTimecodeNow() {
    if (!m_adapter) return;

    m_timecodeTimer.stop();
    broadcastJson(ControlState::timecodeMessage(*m_adapter));
}

void ControlWebSocketServer::scheduleTimecode() {
    if (!m_timecodeTimer.isActive()) {
        m_timecodeTimer.start(100);
    }
}

void ControlWebSocketServer::handleNewConnection() {
    QWebSocket* socket = m_server->nextPendingConnection();
    if (!socket) {
        qWarning() << "WebSocket control API newConnection without pending socket";
        return;
    }

    qInfo() << "WebSocket control API accepted client" << socket->peerAddress().toString()
            << socket->peerPort() << socket->requestUrl().toString();
    const QString clientId = QString::number(++m_nextClientSerial);
    socket->setProperty("controlClientId", clientId);
    m_sockets.insert(socket);
    m_clientsById.insert(clientId, socket);

    connect(socket, &QWebSocket::textMessageReceived, this,
            &ControlWebSocketServer::handleTextMessage);
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            &ControlWebSocketServer::handleBinaryMessage);
    connect(socket, &QWebSocket::disconnected, this,
            &ControlWebSocketServer::handleSocketDisconnected);

    sendJson(ControlState::snapshotMessage(*m_adapter), socket);
    sendJson(ControlState::timecodeMessage(*m_adapter), socket);
}

void ControlWebSocketServer::handleTextMessage(const QString& message) {
    auto socket = qobject_cast<QWebSocket*>(sender());
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
        sendJson(ControlProtocol::ackError(parsed.message.id, validated.code, validated.message),
                 socket);
        return;
    }

    QJsonObject commandArgs = validated.normalizedArgs;
    commandArgs.insert(QStringLiteral("_clientId"), socket->property("controlClientId").toString());
    commandArgs.insert(QStringLiteral("_commandId"), parsed.message.id);

    const auto result = m_adapter->executeCommand(parsed.message.name, commandArgs);
    sendJson(result.ok ? ControlProtocol::ack(parsed.message.id, result.details)
                       : ControlProtocol::ackError(parsed.message.id, result.code, result.message),
             socket);
}

void ControlWebSocketServer::handleBinaryMessage(const QByteArray&) {
    auto socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    sendJson(ControlProtocol::error(QStringLiteral("unsupported_message"),
                                    QStringLiteral("Only text messages are supported")),
             socket);
}

void ControlWebSocketServer::handleSocketDisconnected() {
    auto socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    const QString clientId = socket->property("controlClientId").toString();

    if (m_adapter) {
        QJsonObject releaseArgs;
        releaseArgs.insert(QStringLiteral("active"), false);
        releaseArgs.insert(QStringLiteral("_clientId"), clientId);
        m_adapter->executeCommand(QStringLiteral("transport.holdSpeed"), releaseArgs);
    }

    m_clientsById.remove(clientId);
    if (m_adapter) m_adapter->notifyClientDisconnected(clientId);

    m_sockets.remove(socket);
    socket->deleteLater();
}

void ControlWebSocketServer::deliverCommandCompleted(const QString& clientId,
                                                     const QString& commandId,
                                                     const QJsonObject& completion) {
    QWebSocket* socket = m_clientsById.value(clientId, nullptr);
    if (!socket) return; // client departed: drop, never re-route

    QJsonObject data = completion;
    data.insert(QStringLiteral("id"), commandId);

    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("event"));
    event.insert(QStringLiteral("name"), QStringLiteral("command.completed"));
    event.insert(QStringLiteral("data"), data);
    sendJson(event, socket);
}

void ControlWebSocketServer::sendJson(const QJsonObject& message, QWebSocket* socket) {
    if (!socket) return;
    socket->sendTextMessage(QString::fromUtf8(ControlProtocol::compact(message)));
}

void ControlWebSocketServer::broadcastJson(const QJsonObject& message) {
    const QByteArray payload = ControlProtocol::compact(message);
    for (QWebSocket* socket : m_sockets) {
        socket->sendTextMessage(QString::fromUtf8(payload));
    }
}
