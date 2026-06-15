#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>

#include "telemetry/telemetryclient.h"

class LocalSseServer : public QObject {
    Q_OBJECT

public:
    explicit LocalSseServer(QObject *parent = nullptr)
        : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            m_socket = m_server.nextPendingConnection();
            connect(m_socket, &QTcpSocket::readyRead, this, &LocalSseServer::handleRequestData);
        });
    }

    bool listen() {
        return m_server.listen(QHostAddress::LocalHost);
    }

    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/events").arg(m_server.serverPort()));
    }

    void setStatusCode(int statusCode) {
        m_statusCode = statusCode;
    }

    void setCloseAfterHeaders(bool closeAfterHeaders) {
        m_closeAfterHeaders = closeAfterHeaders;
    }

    void writeEvent(const QByteArray &event) {
        QVERIFY(m_socket);
        m_socket->write(event);
        QVERIFY(m_socket->flush());
    }

signals:
    void requestReceived();

private slots:
    void handleRequestData() {
        m_requestBuffer.append(m_socket->readAll());
        if (!m_requestBuffer.contains("\r\n\r\n")) return;

        const QByteArray reason = m_statusCode == 200 ? QByteArray("OK") : QByteArray("Internal Server Error");
        QByteArray response = "HTTP/1.1 " + QByteArray::number(m_statusCode) + ' ' + reason + "\r\n";
        response += "Content-Type: text/event-stream\r\n";
        response += "Cache-Control: no-cache\r\n";
        response += "\r\n";
        m_socket->write(response);
        QVERIFY(m_socket->flush());
        emit requestReceived();

        if (m_closeAfterHeaders) {
            m_socket->disconnectFromHost();
        }
    }

private:
    QTcpServer m_server;
    QTcpSocket *m_socket = nullptr;
    QByteArray m_requestBuffer;
    int m_statusCode = 200;
    bool m_closeAfterHeaders = false;
};

class TestTelemetryClient : public QObject {
    Q_OBJECT

private slots:
    void http500DoesNotEmitConnectedTrueAndSetsError();
    void successfulSseEmitsConnectedTrueAndTelemetryEvent();
    void validEventAfterParserErrorClearsLastError();
};

void TestTelemetryClient::http500DoesNotEmitConnectedTrueAndSetsError() {
    LocalSseServer server;
    QVERIFY(server.listen());
    server.setStatusCode(500);
    server.setCloseAfterHeaders(true);

    TelemetryClient client;
    QSignalSpy connectedSpy(&client, &TelemetryClient::connectedChanged);
    QSignalSpy errorSpy(&client, &TelemetryClient::errorOccurred);
    QSignalSpy requestSpy(&server, &LocalSseServer::requestReceived);

    client.start(server.url());
    QCOMPARE(connectedSpy.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(requestSpy.count(), 1, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
    QCOMPARE(connectedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(client.lastError().contains(QStringLiteral("HTTP 500")));
}

void TestTelemetryClient::successfulSseEmitsConnectedTrueAndTelemetryEvent() {
    LocalSseServer server;
    QVERIFY(server.listen());

    TelemetryClient client;
    QSignalSpy connectedSpy(&client, &TelemetryClient::connectedChanged);
    QSignalSpy requestSpy(&server, &LocalSseServer::requestReceived);
    QList<TelemetryEvent> events;
    connect(&client, &TelemetryClient::telemetryEvent, this, [&events](const TelemetryEvent &event) {
        events.append(event);
    });

    client.start(server.url());
    QCOMPARE(connectedSpy.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(requestSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);
    QCOMPARE(connectedSpy.takeFirst().at(0).toBool(), true);

    server.writeEvent("event: telemetry\n"
                      "data: {\"feedId\":\"cam-main\",\"status\":\"ok\"}\n"
                      "\n");

    QTRY_COMPARE_WITH_TIMEOUT(events.size(), 1, 2000);
    QCOMPARE(events[0].feedId, QStringLiteral("cam-main"));
    QCOMPARE(events[0].eventType, QStringLiteral("telemetry"));
}

void TestTelemetryClient::validEventAfterParserErrorClearsLastError() {
    LocalSseServer server;
    QVERIFY(server.listen());

    TelemetryClient client;
    QSignalSpy connectedSpy(&client, &TelemetryClient::connectedChanged);
    QSignalSpy errorSpy(&client, &TelemetryClient::errorOccurred);
    QSignalSpy requestSpy(&server, &LocalSseServer::requestReceived);
    QList<TelemetryEvent> events;
    connect(&client, &TelemetryClient::telemetryEvent, this, [&events](const TelemetryEvent &event) {
        events.append(event);
    });

    client.start(server.url());
    QCOMPARE(connectedSpy.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(requestSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(connectedSpy.count(), 1, 2000);

    server.writeEvent("data: { nope }\n\n");
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 2000);
    QVERIFY(client.lastError().contains(QStringLiteral("JSON")));

    server.writeEvent("data: {\"feedId\":\"cam-main\",\"status\":\"ok\"}\n\n");
    QTRY_COMPARE_WITH_TIMEOUT(events.size(), 1, 2000);
    QVERIFY(client.lastError().isEmpty());
}

QTEST_GUILESS_MAIN(TestTelemetryClient)
#include "tst_telemetryclient.moc"
