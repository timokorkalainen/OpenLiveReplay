#include "project/projectimportclient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

ProjectImportClient::ProjectImportClient(QObject *parent)
    : QObject(parent),
      m_networkAccessManager(new QNetworkAccessManager(this)) {
}

ProjectImportClient::~ProjectImportClient() {
    clearReply();
}

void ProjectImportClient::fetch(const QUrl &url) {
    clearReply();

    if (!isAllowedUrl(url)) {
        emit failed(QStringLiteral("Import settings URL must be a valid HTTPS URL"));
        return;
    }
    m_requestedUrl = url.toString();

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_networkAccessManager->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &ProjectImportClient::onFinished);
}

void ProjectImportClient::onFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) {
        if (reply) reply->deleteLater();
        return;
    }

    m_reply = nullptr;

    const QUrl finalUrl = reply->url();
    const QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!isAllowedUrl(finalUrl)) {
        emit failed(QStringLiteral("Import settings URL redirected to a non-HTTPS URL"));
    } else if (statusCode.isValid() && statusCode.toInt() >= 400) {
        const int status = statusCode.toInt();
        const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        QString message = QStringLiteral("HTTP %1").arg(status);
        if (!reason.isEmpty()) {
            message += QStringLiteral(": ") + reason;
        }
        emit failed(message);
    } else if (reply->error() != QNetworkReply::NoError) {
        emit failed(reply->errorString());
    } else {
        emit finished(reply->readAll(), m_requestedUrl);
    }

    reply->deleteLater();
    m_requestedUrl.clear();
}

bool ProjectImportClient::isAllowedUrl(const QUrl &url) const {
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty();
}

void ProjectImportClient::cancel() {
    clearReply();
}

void ProjectImportClient::clearReply() {
    if (!m_reply) {
        m_requestedUrl.clear();
        return;
    }

    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    m_requestedUrl.clear();
}
