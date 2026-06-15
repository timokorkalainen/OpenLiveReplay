#ifndef TELEMETRYCLIENT_H
#define TELEMETRYCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QString>

#include "telemetry/sseparser.h"
#include "telemetry/telemetryevent.h"

class QNetworkReply;
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
    void onReadyRead();
    void onFinished();
    void onError();

private:
    void setLastError(const QString &message);

    QNetworkAccessManager m_networkAccessManager;
    QNetworkReply *m_reply = nullptr;
    SseParser m_parser;
    QString m_lastError;
};

#endif // TELEMETRYCLIENT_H
