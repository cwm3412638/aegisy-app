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

                    if (path == QStringLiteral("/v1/chat/completions")) {
                        if (!body.value(QStringLiteral("stream")).toBool()) {
                            ++presentationRequestCount;
                            const QString plan = presentationRequestCount == 1
                                ? QStringLiteral(
                                    "{\"title\":\"测试演示\" \"subtitle\":\"Aegisy\","
                                    "\"slides\":[{\"layout\":\"bullets\",\"title\":\"概览\","
                                    "\"bullets\":[\"第一点\"]}]}")
                                : QStringLiteral(
                                    "{\"title\":\"测试演示\",\"subtitle\":\"Aegisy\","
                                    "\"theme\":\"swiss\",\"slides\":[{\"layout\":\"bullets\","
                                    "\"title\":\"概览\",\"bullets\":[\"第一点\"]}]}" );
                            const QByteArray payload = QJsonDocument(QJsonObject{
                                { QStringLiteral("choices"), QJsonArray{ QJsonObject{
                                    { QStringLiteral("message"), QJsonObject{
                                        { QStringLiteral("role"), QStringLiteral("assistant") },
                                        { QStringLiteral("content"), plan }
                                    }}
                                }} }
                            }).toJson(QJsonDocument::Compact);
                            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                          + QByteArray::number(payload.size())
                                          + "\r\nConnection: close\r\n\r\n" + payload);
                            socket->disconnectFromHost();
                            return;
                        }
                        QByteArray payload;
                        if (chatResponseMode == 1) {
                            payload =
                                "data: {\"choices\":[{\"delta\":{\"content\":\"尾部\"}}]}\n\n"
                                "data: [DONE]";
                        } else if (chatResponseMode == 2) {
                            payload =
                                "data: {\"choices\":[{\"delta\":{\"content\":\"部分\"}}]}\n\n";
                        } else {
                            payload =
                                "data: {\"choices\":[{\"delta\":{\"content\":\"\\u4f60\\u597d\"}}]}\n\n"
                                "data: {\"choices\":[{\"delta\":{\"content\":\"！\"}}]}\n\n"
                                "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3,\"total_tokens\":15}}\n\n"
                                "data: [DONE]\n\n";
                        }
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                                      + QByteArray::number(payload.size())
                                      + "\r\nConnection: close\r\n\r\n" + payload);
                        socket->disconnectFromHost();
                        return;
                    }

                    if (path == QStringLiteral("/v1/images/generations")) {
                        const QByteArray encoded = QByteArray("aegisy-image-bytes").toBase64();
                        const QByteArray payload =
                            "data: {\"type\":\"image_generation.partial\",\"partial_image_b64\":\"cGFydGlhbA==\"}\n\n"
                            "data: {\"type\":\"image_generation.completed\",\"response\":{\"output\":[{\"result\":\""
                            + encoded
                            + "\",\"output_format\":\"png\",\"revised_prompt\":\"Aegisy test image\"}]}}\n\n"
                              "data: [DONE]\n\n";
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                                      + QByteArray::number(payload.size())
                                      + "\r\nConnection: close\r\n\r\n" + payload);
                        socket->disconnectFromHost();
                        return;
                    }

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
        presentationRequestCount = 0;
        chatResponseMode = 0;
    }

    QTcpServer server;
    QString method;
    QString path;
    QJsonObject body;
    int presentationRequestCount = 0;
    int chatResponseMode = 0;
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

    int rawKeySignalCount = 0;
    QString projectionFailure;
    QObject::connect(&client, &ApiClient::apiKeysReceived, &client,
                     [&](const QJsonArray &) { ++rawKeySignalCount; });
    QObject::connect(&client, &ApiClient::companionConfigurationFailed, &client,
                     [&](const QString &code) { projectionFailure = code; });
    client.getApiKeys();
    if (!require(projectionFailure == QStringLiteral("projection-account-unverified"),
                 "unverified account was allowed to request website Keys")
            || !require(rawKeySignalCount == 0,
                        "unverified account published raw website Keys")
            || !require(server.method.isEmpty(),
                        "unverified account contacted the website Keys endpoint")) {
        return 1;
    }

    QString modelFailure;
    int companionModelSuccessCount = 0;
    QObject::connect(&client, &ApiClient::companionModelsFailed, &client,
                     [&](const QString &, const QString &, const QString &code) {
        modelFailure = code;
    });
    QObject::connect(&client, &ApiClient::companionModelsReceived, &client,
                     [&](const QString &, const QString &, const QJsonObject &) {
        ++companionModelSuccessCount;
    });
    client.getCompanionModels(
        QStringLiteral("model-request-1"),
        QStringLiteral("website-account-session:sha256:") + QString(64, QLatin1Char('a')),
        QStringLiteral("website-key:sha256:") + QString(64, QLatin1Char('b')),
        QStringLiteral("website-credential:sha256:") + QString(64, QLatin1Char('c')),
        QString(64, QLatin1Char('d')), QStringLiteral("openai"));
    if (!require(modelFailure == QStringLiteral("companion-model-binding-invalid"),
                 "unverified account was allowed to request companion models")
            || !require(companionModelSuccessCount == 0,
                        "unverified companion model request produced a result")
            || !require(server.method.isEmpty(),
                        "unverified companion model request contacted the provider")) {
        return 1;
    }

    QString policyFailure;
    QObject::connect(&client, &ApiClient::workbenchEmergencyPolicyFailed, &client,
                     [&](const QString &code) { policyFailure = code; });
    client.getWorkbenchEmergencyPolicy();
    if (!require(policyFailure == QStringLiteral("policy-origin-untrusted"),
                 "emergency policy accepted an unauthenticated HTTP origin")
        || !require(server.method.isEmpty(),
                    "emergency policy contacted an HTTP origin")) return 1;
    policyFailure.clear();
    client.setBaseUrl(QStringLiteral("https://www.aegisy.cc"));
    client.setAuthToken(QString());
    client.getWorkbenchEmergencyPolicy();
    if (!require(policyFailure == QStringLiteral("policy-auth-unavailable"),
                 "emergency policy request did not require authentication")) return 1;
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

    server.clear();
    succeeded = false;
    QString streamedContent;
    int promptTokens = 0;
    int completionTokens = 0;
    int totalTokens = 0;
    const QJsonArray messages{ QJsonObject{
        { QStringLiteral("role"), QStringLiteral("user") },
        { QStringLiteral("content"), QStringLiteral("你好") }
    }};
    if (!waitFor([&]() {
            client.sendChatMessage(QStringLiteral("chat-test"), QStringLiteral("sk-chat-test"),
                                   QStringLiteral("gpt-test"), messages);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::chatChunkReceived, &loop,
                [&](const QString &requestId, const QString &chunk) {
                    if (requestId == QStringLiteral("chat-test")) streamedContent += chunk;
                });
            QObject::connect(&client, &ApiClient::chatUsageReceived, &loop,
                [&](const QString &requestId, int prompt, int completion, int total) {
                    if (requestId == QStringLiteral("chat-test")) {
                        promptTokens = prompt;
                        completionTokens = completion;
                        totalTokens = total;
                    }
                });
            QObject::connect(&client, &ApiClient::chatCompleted, &loop,
                [&](const QString &requestId, const QString &content) {
                    succeeded = requestId == QStringLiteral("chat-test")
                        && content == QStringLiteral("你好！");
                    loop.quit();
                });
            QObject::connect(&client, &ApiClient::chatFailed, &loop,
                [&](const QString &, const QString &) { loop.quit(); });
        })
        || !require(succeeded, "streaming chat request failed")
        || !require(streamedContent == QStringLiteral("你好！"), "chat chunks mismatch")
        || !require(promptTokens == 12 && completionTokens == 3 && totalTokens == 15,
                    "chat usage mismatch")
        || !require(server.method == QStringLiteral("POST"), "chat method mismatch")
        || !require(server.path == QStringLiteral("/v1/chat/completions"), "chat path mismatch")
        || !require(server.body.value(QStringLiteral("model")).toString() == QStringLiteral("gpt-test"),
                    "chat model mismatch")
        || !require(server.body.value(QStringLiteral("stream")).toBool(), "chat stream flag mismatch")
        || !require(server.body.value(QStringLiteral("stream_options")).toObject()
                        .value(QStringLiteral("include_usage")).toBool(),
                    "chat usage option mismatch")
        || !require(server.body.value(QStringLiteral("messages")).toArray().size() == 1,
                    "chat messages mismatch")) return 1;

    server.clear();
    server.chatResponseMode = 1;
    succeeded = false;
    streamedContent.clear();
    QString trailingDoneFailure;
    if (!waitFor([&]() {
            client.sendChatMessage(QStringLiteral("chat-trailing-done"), QStringLiteral("sk-chat-test"),
                                   QStringLiteral("gpt-test"), messages);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::chatCompleted, &loop,
                [&](const QString &requestId, const QString &content) {
                    succeeded = requestId == QStringLiteral("chat-trailing-done")
                        && content == QStringLiteral("尾部");
                    loop.quit();
                });
            QObject::connect(&client, &ApiClient::chatFailed, &loop,
                [&](const QString &, const QString &error) {
                    trailingDoneFailure = error;
                    loop.quit();
                });
        })
        || !require(succeeded, "SSE final data without newline was not completed")
        || !require(trailingDoneFailure.isEmpty(), "SSE final data without newline unexpectedly failed")) return 1;

    server.clear();
    server.chatResponseMode = 2;
    succeeded = false;
    QString truncatedFailure;
    if (!waitFor([&]() {
            client.sendChatMessage(QStringLiteral("chat-truncated"), QStringLiteral("sk-chat-test"),
                                   QStringLiteral("gpt-test"), messages);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::chatCompleted, &loop,
                [&](const QString &requestId, const QString &) {
                    if (requestId == QStringLiteral("chat-truncated")) succeeded = true;
                    loop.quit();
                });
            QObject::connect(&client, &ApiClient::chatFailed, &loop,
                [&](const QString &requestId, const QString &error) {
                    if (requestId == QStringLiteral("chat-truncated")) {
                        truncatedFailure = error;
                        loop.quit();
                    }
                });
        })
        || !require(!succeeded, "truncated SSE stream was reported complete")
        || !require(truncatedFailure == QStringLiteral("stream disconnected before completion"),
                    "truncated SSE stream did not fail with the stable error")) return 1;

    server.clear();
    succeeded = false;
    QByteArray generatedImage;
    QString generatedFormat;
    QString revisedPrompt;
    if (!waitFor([&]() {
            client.generateImage(QStringLiteral("sk-image-test"), QStringLiteral("gpt-image-2"),
                                 QStringLiteral("生成测试图片"), QStringLiteral("1024x1024"),
                                 QStringLiteral("auto"), QStringLiteral("png"));
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::imageGenerated, &loop,
                [&](const QByteArray &data, const QString &format, const QString &revised) {
                    generatedImage = data;
                    generatedFormat = format;
                    revisedPrompt = revised;
                    succeeded = true;
                    loop.quit();
                });
            QObject::connect(&client, &ApiClient::imageGenerationFailed, &loop,
                [&](const QString &) { loop.quit(); });
        })
        || !require(succeeded, "streaming image request failed")
        || !require(generatedImage == QByteArray("aegisy-image-bytes"), "image data mismatch")
        || !require(generatedFormat == QStringLiteral("png"), "image format mismatch")
        || !require(revisedPrompt == QStringLiteral("Aegisy test image"), "revised prompt mismatch")
        || !require(server.path == QStringLiteral("/v1/images/generations"), "image path mismatch")
        || !require(server.body.value(QStringLiteral("stream")).toBool(), "image stream flag mismatch")
        || !require(server.body.value(QStringLiteral("response_format")).toString()
                        == QStringLiteral("b64_json"), "image response format mismatch")) return 1;

    server.clear();
    succeeded = false;
    if (!waitFor([&]() {
            client.requestPresentationPlan(QStringLiteral("ppt-test"),
                                           QStringLiteral("sk-ppt-test"),
                                           QStringLiteral("gpt-test"),
                                           QStringLiteral("制作测试 PPT"));
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::presentationPlanReceived, &loop,
                [&](const QString &requestId, const QJsonObject &plan) {
                    succeeded = requestId == QStringLiteral("ppt-test")
                        && plan.value(QStringLiteral("title")).toString() == QStringLiteral("测试演示")
                        && plan.value(QStringLiteral("slides")).toArray().size() == 1;
                    loop.quit();
                });
            QObject::connect(&client, &ApiClient::presentationPlanFailed, &loop,
                [&](const QString &, const QString &) { loop.quit(); });
        })
        || !require(succeeded, "presentation plan request failed")
        || !require(server.path == QStringLiteral("/v1/chat/completions"),
                    "presentation plan path mismatch")
        || !require(!server.body.value(QStringLiteral("stream")).toBool(),
                    "presentation plan must be non-streaming")
        || !require(server.presentationRequestCount == 2,
                    "presentation repair retry was not used")
        || !require(server.body.value(QStringLiteral("response_format")).toObject()
                        .value(QStringLiteral("type")).toString() == QStringLiteral("json_object"),
                    "presentation response format mismatch")
        || !require(server.body.value(QStringLiteral("messages")).toArray().size() == 2,
                    "presentation plan messages mismatch")) return 1;

    return 0;
}
