#include "api_client.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <iostream>

class MockHttpServer : public QObject
{
public:
    explicit MockHttpServer(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    buffer->append(socket->readAll());
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray headers = buffer->left(headerEnd);
                    int contentLength = 0;
                    for (const QByteArray &line : headers.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                        }
                    }
                    if (buffer->size() < headerEnd + 4 + contentLength) return;
                    const QList<QByteArray> requestLine = headers.split('\n').first().trimmed().split(' ');
                    if (requestLine.size() >= 2) {
                        method = QString::fromLatin1(requestLine[0]);
                        path = QString::fromLatin1(requestLine[1]);
                    }
                    body = QJsonDocument::fromJson(
                        buffer->mid(headerEnd + 4, contentLength)).object();

                    QJsonValue data = QJsonObject();
                    if (path == QStringLiteral("/api/v1/groups/available")) {
                        data = QJsonArray{ QJsonObject{
                            { QStringLiteral("id"), 7 },
                            { QStringLiteral("name"), QStringLiteral("Codex") }
                        }};
                    } else if (path == QStringLiteral("/api/v1/redeem")) {
                        data = QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("balance") },
                            { QStringLiteral("value"), 10 },
                            { QStringLiteral("new_balance"), 20 }
                        };
                    }
                    const QByteArray payload = QJsonDocument(QJsonObject{
                        { QStringLiteral("code"), 0 },
                        { QStringLiteral("data"), data }
                    }).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n"
                                  + payload);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool listen()
    {
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }

    void clear()
    {
        method.clear();
        path.clear();
        body = QJsonObject();
    }

    QTcpServer server;
    QString method;
    QString path;
    QJsonObject body;
};

namespace {

bool waitFor(const std::function<void()> &start,
             const std::function<void(QEventLoop &)> &connectResult)
{
    QEventLoop loop;
    bool timedOut = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    connectResult(loop);
    timer.start(3000);
    start();
    loop.exec();
    return !timedOut;
}

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    MockHttpServer server;
    if (!server.listen()) return 1;
    ApiClient client;
    client.setBaseUrl(server.baseUrl());
    client.setAuthToken(QStringLiteral("test-token"));

    bool succeeded = false;
    if (!waitFor([&]() { client.changePassword(QStringLiteral("old-pass"), QStringLiteral("new-pass-123")); },
            [&](QEventLoop &loop) {
                QObject::connect(&client, &ApiClient::passwordChanged, &loop, [&]() {
                    succeeded = true; loop.quit();
                });
                QObject::connect(&client, &ApiClient::passwordChangeFailed, &loop,
                                 [&](const QString &) { loop.quit(); });
            })
        || !require(succeeded, "password request failed")
        || !require(server.method == QStringLiteral("PUT"), "password method mismatch")
        || !require(server.path == QStringLiteral("/api/v1/user/password"), "password path mismatch")
        || !require(server.body.value(QStringLiteral("old_password")).toString() == QStringLiteral("old-pass"),
                    "old password field mismatch")
        || !require(server.body.value(QStringLiteral("new_password")).toString() == QStringLiteral("new-pass-123"),
                    "new password field mismatch")) return 1;

    server.clear();
    succeeded = false;
    if (!waitFor([&]() { client.redeemCode(QStringLiteral("CARD-123")); },
            [&](QEventLoop &loop) {
                QObject::connect(&client, &ApiClient::redeemCompleted, &loop,
                                 [&](const QJsonObject &) { succeeded = true; loop.quit(); });
                QObject::connect(&client, &ApiClient::redeemFailed, &loop,
                                 [&](const QString &) { loop.quit(); });
            })
        || !require(succeeded, "redeem request failed")
        || !require(server.method == QStringLiteral("POST"), "redeem method mismatch")
        || !require(server.path == QStringLiteral("/api/v1/redeem"), "redeem path mismatch")
        || !require(server.body.value(QStringLiteral("code")).toString() == QStringLiteral("CARD-123"),
                    "redeem code field mismatch")) return 1;

    server.clear();
    succeeded = false;
    if (!waitFor([&]() { client.getGroups(); }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::groupsReceived, &loop,
                             [&](const QJsonArray &groups) { succeeded = groups.size() == 1; loop.quit(); });
        })
        || !require(succeeded, "groups request failed")
        || !require(server.path == QStringLiteral("/api/v1/groups/available"), "groups path mismatch")) return 1;

    const QJsonObject keyPayload{
        { QStringLiteral("name"), QStringLiteral("Desktop Key") },
        { QStringLiteral("group_id"), 7 },
        { QStringLiteral("quota"), 0 }
    };
    for (const QString &action : { QStringLiteral("create"), QStringLiteral("update"),
                                   QStringLiteral("delete") }) {
        server.clear();
        succeeded = false;
        const bool completed = waitFor([&]() {
            if (action == QStringLiteral("create")) client.createApiKey(keyPayload);
            else if (action == QStringLiteral("update")) client.updateApiKey(QStringLiteral("42"), keyPayload);
            else client.deleteApiKey(QStringLiteral("42"));
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::apiKeyOperationCompleted, &loop,
                [&](const QString &receivedAction, const QJsonObject &) {
                    if (receivedAction == action) succeeded = true;
                    loop.quit();
                });
            QObject::connect(&client, &ApiClient::apiKeyOperationFailed, &loop,
                [&](const QString &, const QString &) { loop.quit(); });
        });
        if (!completed || !require(succeeded, "key operation failed")) return 1;
        const QString expectedMethod = action == QStringLiteral("create") ? QStringLiteral("POST")
            : action == QStringLiteral("update") ? QStringLiteral("PUT") : QStringLiteral("DELETE");
        const QString expectedPath = action == QStringLiteral("create")
            ? QStringLiteral("/api/v1/keys") : QStringLiteral("/api/v1/keys/42");
        if (!require(server.method == expectedMethod, "key method mismatch")
                || !require(server.path == expectedPath, "key path mismatch")) return 1;
    }
    return 0;
}
