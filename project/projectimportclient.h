#ifndef PROJECTIMPORTCLIENT_H
#define PROJECTIMPORTCLIENT_H

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

class ProjectImportClient : public QObject {
    Q_OBJECT

public:
    explicit ProjectImportClient(QObject *parent = nullptr);
    ~ProjectImportClient() override;

    void fetch(const QUrl &url);

signals:
    void finished(const QByteArray &body, const QString &finalUrl);
    void failed(const QString &message);

private slots:
    void onFinished();

private:
    bool isAllowedUrl(const QUrl &url) const;
    void clearReply();

    QNetworkAccessManager *m_networkAccessManager = nullptr;
    QNetworkReply *m_reply = nullptr;
};

#endif // PROJECTIMPORTCLIENT_H
