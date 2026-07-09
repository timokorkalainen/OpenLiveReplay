#ifndef CONTROLWEBSOCKETSERVER_H
#define CONTROLWEBSOCKETSERVER_H

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTimer>

class QWebSocket;
class QWebSocketServer;

class ControlApiAdapter;

class ControlWebSocketServer : public QObject {
    Q_OBJECT

public:
    explicit ControlWebSocketServer(ControlApiAdapter* adapter, QObject* parent = nullptr);
    ~ControlWebSocketServer() override;

    bool listen(const QHostAddress& address = QHostAddress::Any, quint16 port = 8115);
    quint16 serverPort() const;
    QString lastError() const;

public slots:
    void publishPatch(const QString& path, const QJsonObject& value = {});
    void publishPatchObject(const QString& path, const QJsonObject& value);
    void publishEvent(const QString& name, const QJsonObject& data = {});
    void publishTimecodeNow();
    void scheduleTimecode();

private slots:
    void handleNewConnection();
    void handleTextMessage(const QString& message);
    void handleBinaryMessage(const QByteArray& message);
    void handleSocketDisconnected();
    void deliverCommandCompleted(const QString& clientId, const QString& commandId,
                                 const QJsonObject& completion);

private:
    void sendJson(const QJsonObject& message, QWebSocket* socket);
    void broadcastJson(const QJsonObject& message);

    ControlApiAdapter* m_adapter;
    QWebSocketServer* m_server;
    QSet<QWebSocket*> m_sockets;
    QHash<QString, QWebSocket*> m_clientsById;
    quint64 m_nextClientSerial = 0;
    QTimer m_timecodeTimer;
    QString m_lastError;
};

#endif
