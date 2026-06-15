#ifndef TELEMETRYCLIENT_H
#define TELEMETRYCLIENT_H

#include <QObject>
#include <QString>

#include "telemetry/sseparser.h"
#include "telemetry/telemetryevent.h"

class QNetworkReply;
class QNetworkAccessManager;
class QUrl;

class TelemetryClient : public QObject {
    Q_OBJECT

public:
    explicit TelemetryClient(QObject *parent = nullptr);
    ~TelemetryClient() override;

    void start(const QUrl &url);
    void stop();

    bool running() const;
    QString lastError() const;

signals:
    void telemetryEvent(const TelemetryEvent &event);
    void connectedChanged(bool connected);
    void errorOccurred(const QString &message);

private slots:
    void onMetaDataChanged();
    void onReadyRead();
    void onFinished();
    void onError();

private:
    bool establishConnectionIfUsable(QNetworkReply *reply, bool allowMissingHttpStatus);
    bool handleHttpErrorStatus(QNetworkReply *reply);
    void establishConnected();
    void clearReply(QNetworkReply *reply);
    void setLastError(const QString &message);

    QNetworkAccessManager *m_networkAccessManager = nullptr;
    QNetworkReply *m_reply = nullptr;
    SseParser m_parser;
    QString m_lastError;
    bool m_connected = false;
};

#endif // TELEMETRYCLIENT_H
