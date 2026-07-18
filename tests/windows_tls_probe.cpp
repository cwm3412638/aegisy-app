#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QUrl url(argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("https://www.aegisy.cc/"));
    QJsonObject report{
        { QStringLiteral("supports_ssl"), QSslSocket::supportsSsl() },
        { QStringLiteral("ssl_build_version"), QSslSocket::sslLibraryBuildVersionString() },
        { QStringLiteral("ssl_runtime_version"), QSslSocket::sslLibraryVersionString() },
        { QStringLiteral("probe_url"), url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment) },
    };
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    report.insert(QStringLiteral("active_backend"), QSslSocket::activeBackend());
    report.insert(QStringLiteral("available_backends"),
                  QJsonArray::fromStringList(QSslSocket::availableBackends()));
#endif
    if (!QSslSocket::supportsSsl()) {
        report.insert(QStringLiteral("ok"), false);
        report.insert(QStringLiteral("error"), QStringLiteral("TLS initialization failed"));
        qCritical().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
        return 2;
    }
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
        report.insert(QStringLiteral("ok"), false);
        report.insert(QStringLiteral("error"), QStringLiteral("probe URL must use HTTPS"));
        qCritical().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
        return 3;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Aegisy-TLS-Probe/1");
    QNetworkReply *reply = manager.get(request);
    QJsonArray sslErrors;
    QObject::connect(reply, &QNetworkReply::sslErrors,
                     [&sslErrors](const QList<QSslError> &errors) {
        for (const QSslError &error : errors) sslErrors.append(error.errorString());
    });

    bool timedOut = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&]() {
        timedOut = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished,
                     &application, &QCoreApplication::quit);
    timeout.start(20000);
    application.exec();
    timeout.stop();

    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = !timedOut && reply->error() == QNetworkReply::NoError
        && status >= 200 && status < 400;
    report.insert(QStringLiteral("ok"), ok);
    report.insert(QStringLiteral("http_status"), status);
    report.insert(QStringLiteral("ssl_errors"), sslErrors);
    if (!ok) {
        report.insert(QStringLiteral("error"), timedOut
            ? QStringLiteral("TLS probe timed out") : reply->errorString());
    }
    const QByteArray output = QJsonDocument(report).toJson(QJsonDocument::Compact);
    if (ok) qInfo().noquote() << output;
    else qCritical().noquote() << output;
    reply->deleteLater();
    return ok ? 0 : 4;
}
