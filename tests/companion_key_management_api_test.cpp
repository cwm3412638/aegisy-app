#include "api_client.h"
#include "companion_config_projection.h"
#include "companion_credential_broker.h"
#include "companion_key_management_projection.h"
#include "secure_storage.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QTimer>

#include <cstring>
#include <functional>
#include <iostream>

struct FakeResponse {
    int status = 200;
    QByteArray body;
    QByteArray contentType = "application/json";
    bool hold = false;
};

class FakeReply final : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, QNetworkAccessManager::Operation operation,
              const FakeResponse &response, QObject *parent)
        : QNetworkReply(parent), m_body(response.body), m_hold(response.hold)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        setHeader(QNetworkRequest::ContentTypeHeader,
                  QString::fromLatin1(response.contentType));
        setHeader(QNetworkRequest::ContentLengthHeader, m_body.size());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (!m_hold) QTimer::singleShot(0, this, [this]() { release(); });
    }

    void abort() override
    {
        if (isFinished()) return;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("cancelled"));
        release();
    }

    void release()
    {
        if (isFinished()) return;
        if (!m_body.isEmpty()) emit readyRead();
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return (m_body.size() - m_offset) + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, m_body.size() - m_offset);
        if (count <= 0) return -1;
        memcpy(data, m_body.constData() + m_offset, static_cast<size_t>(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
    bool m_hold = false;
};

struct CapturedRequest {
    QNetworkAccessManager::Operation operation;
    QUrl url;
    QByteArray body;
};

class FakeNetworkManager final : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

    void enqueue(const FakeResponse &response) { responses.enqueue(response); }

    QQueue<FakeResponse> responses;
    QList<CapturedRequest> requests;
    FakeReply *heldReply = nullptr;

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        CapturedRequest captured{operation, request.url(), {}};
        if (outgoingData) captured.body = outgoingData->readAll();
        requests.append(captured);
        const FakeResponse response = responses.isEmpty()
            ? FakeResponse{500, QByteArrayLiteral("{\"code\":500}"),
                           QByteArrayLiteral("application/json"), false}
            : responses.dequeue();
        auto *reply = new FakeReply(request, operation, response, this);
        if (response.hold) heldReply = reply;
        return reply;
    }
};

class CompanionKeyManagementApiTestAccess
{
public:
    static QString install(ApiClient &client, FakeNetworkManager *manager,
                           const QJsonArray &rawKeys)
    {
        client.setBaseUrl(QStringLiteral("https://www.aegisy.cc"));
        client.setAuthToken(QStringLiteral("test-login-token"));
        client.m_networkManager = manager;
        const QString account = CompanionConfigProjection::accountIdentityForWebsiteId(
            QStringLiteral("management-api-account"));
        client.m_verifiedCompanionAccountIdentity = account;
        client.m_verifiedAccountAuthGeneration = client.m_authGeneration;
        QString error;
        const QJsonObject base = CompanionConfigProjection::fromWebsiteApiKeys(
            rawKeys, account, client.m_baseUrl, 1770000000000LL, &error);
        client.m_currentCompanionProjection = CompanionCredentialBroker::stage(
            rawKeys, base, &error);
        client.m_companionUsageSources.clear();
        const QJsonArray projected = client.m_currentCompanionProjection.value(
            QStringLiteral("keys")).toArray();
        for (int index = 0; index < rawKeys.size(); ++index) {
            const QJsonObject raw = rawKeys.at(index).toObject();
            ApiClient::CompanionUsageSource source;
            source.rawKeyId = raw.value(QStringLiteral("id"));
            source.rawLookupKey = raw.value(QStringLiteral("id")).toString();
            source.keyIdentity = projected.at(index).toObject().value(
                QStringLiteral("key_identity")).toString();
            source.credentialHandle = projected.at(index).toObject().value(
                QStringLiteral("credential_handle")).toString();
            source.rawGroupId = raw.value(QStringLiteral("group_id"))
                .toVariant().toLongLong();
            source.quota = raw.value(QStringLiteral("quota")).toDouble();
            source.quotaUsed = raw.value(QStringLiteral("quota_used")).toDouble();
            source.createdAt = raw.value(QStringLiteral("created_at")).toString();
            client.m_companionUsageSources.append(source);
        }
        client.m_currentCompanionKeyManagementProjection = QJsonObject();
        client.m_currentCompanionGroupSources.clear();
        return account;
    }

    static QString configurationSha(const ApiClient &client)
    {
        return client.m_currentCompanionProjection.value(
            QStringLiteral("projection_sha256")).toString();
    }
};

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool waitFor(const std::function<void()> &start,
             const std::function<void(QEventLoop &)> &connectResult)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    connectResult(loop);
    timer.start(2000);
    start();
    loop.exec();
    return !timedOut;
}

QByteArray response(const QJsonValue &data, const QJsonValue &code = 0)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("code"), code}, {QStringLiteral("data"), data}
    }).toJson(QJsonDocument::Compact);
}

QJsonArray rawKeys(const QString &credential = QStringLiteral("sk-management-api-secret"))
{
    return QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("key/42?unsafe")},
        {QStringLiteral("key"), credential},
        {QStringLiteral("name"), QStringLiteral("Managed Codex")},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("group_id"), 7},
        {QStringLiteral("quota"), 1000},
        {QStringLiteral("quota_used"), 10},
        {QStringLiteral("created_at"), QStringLiteral("2026-08-01T00:00:00Z")},
        {QStringLiteral("group"), QJsonObject{
            {QStringLiteral("id"), 7}, {QStringLiteral("name"), QStringLiteral("Codex")},
            {QStringLiteral("platform"), QStringLiteral("openai")}
        }}
    }};
}

QJsonArray groups()
{
    return QJsonArray{QJsonObject{
        {QStringLiteral("id"), 7}, {QStringLiteral("name"), QStringLiteral("Codex")},
        {QStringLiteral("platform"), QStringLiteral("openai")}
    }};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (!SecureStorage::isAvailable()) return 0;

    ApiClient client;
    auto *manager = new FakeNetworkManager(&client);
    const QString account = CompanionKeyManagementApiTestAccess::install(
        client, manager, rawKeys());
    QString configSha = CompanionKeyManagementApiTestAccess::configurationSha(client);

    QJsonObject firstProjection;
    manager->enqueue(FakeResponse{200, response(groups())});
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-1"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id, const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-1")) {
                    firstProjection = projection;
                    loop.quit();
                }
            });
        }) || !require(CompanionKeyManagementProjection::validate(firstProjection),
                       "first management read failed")) return 1;

    QJsonObject secondProjection;
    manager->enqueue(FakeResponse{200, response(groups())});
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-2"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id, const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-2")) {
                    secondProjection = projection;
                    loop.quit();
                }
            });
        }) || !require(firstProjection.value(QStringLiteral("projection_sha256"))
                           != secondProjection.value(QStringLiteral("projection_sha256")),
                       "management handles did not rotate")) return 1;

    const QJsonObject firstKey = firstProjection.value(QStringLiteral("keys"))
        .toArray().at(0).toObject();
    const QJsonObject secondKey = secondProjection.value(QStringLiteral("keys"))
        .toArray().at(0).toObject();
    const int requestsBeforeStale = manager->requests.size();
    QString staleFailure;
    QObject::connect(&client, &ApiClient::companionKeyOperationFailed, &client,
                     [&](const QString &id, const QString &, const QString &error) {
        if (id == QStringLiteral("stale-update")) staleFailure = error;
    });
    client.updateCompanionApiKey(
        QStringLiteral("stale-update"), account,
        firstKey.value(QStringLiteral("key_identity")).toString(),
        firstKey.value(QStringLiteral("update_handle")).toString(), configSha,
        firstProjection.value(QStringLiteral("projection_sha256")).toString(),
        QJsonObject{{QStringLiteral("status"), QStringLiteral("inactive")}});
    if (!require(!staleFailure.isEmpty()
                    && manager->requests.size() == requestsBeforeStale,
                 "stale update handle reached the network")) return 1;

    bool updateCompleted = false;
    manager->enqueue(FakeResponse{200, response(QJsonObject())});
    if (!waitFor([&]() {
            client.updateCompanionApiKey(
                QStringLiteral("valid-update"), account,
                secondKey.value(QStringLiteral("key_identity")).toString(),
                secondKey.value(QStringLiteral("update_handle")).toString(), configSha,
                secondProjection.value(QStringLiteral("projection_sha256")).toString(),
                QJsonObject{{QStringLiteral("status"), QStringLiteral("inactive")}});
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyOperationCompleted,
                             &loop, [&](const QString &id, const QString &action, bool) {
                if (id == QStringLiteral("valid-update")
                        && action == QStringLiteral("update")) {
                    updateCompleted = true;
                    loop.quit();
                }
            });
        }) || !require(updateCompleted, "valid update did not complete")) return 1;
    const CapturedRequest update = manager->requests.last();
    if (!require(update.operation == QNetworkAccessManager::CustomOperation,
                 "update did not use a custom PUT request")
            || !require(update.url.toString(QUrl::FullyEncoded).contains(
                            QStringLiteral("key%2F42%3Funsafe")),
                        "raw Key ID was not encoded as one path segment")
            || !require(QJsonDocument::fromJson(update.body).object().value(
                            QStringLiteral("status")) == QJsonValue("inactive"),
                        "update payload changed")) return 1;

    CompanionKeyManagementApiTestAccess::install(client, manager, rawKeys());
    configSha = CompanionKeyManagementApiTestAccess::configurationSha(client);
    manager->enqueue(FakeResponse{200, response(groups())});
    QJsonObject strictCodeProjection;
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-code"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id, const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-code")) {
                    strictCodeProjection = projection;
                    loop.quit();
                }
            });
        })) return 1;
    const QJsonObject strictCodeKey = strictCodeProjection.value(
        QStringLiteral("keys")).toArray().at(0).toObject();
    QString strictCodeFailure;
    manager->enqueue(FakeResponse{
        200, response(QJsonObject(), QStringLiteral("500"))});
    if (!waitFor([&]() {
            client.updateCompanionApiKey(
                QStringLiteral("strict-code-update"), account,
                strictCodeKey.value(QStringLiteral("key_identity")).toString(),
                strictCodeKey.value(QStringLiteral("update_handle")).toString(),
                configSha, strictCodeProjection.value(
                    QStringLiteral("projection_sha256")).toString(),
                QJsonObject{{QStringLiteral("status"), QStringLiteral("inactive")}});
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyOperationFailed,
                             &loop, [&](const QString &id, const QString &,
                                        const QString &error) {
                if (id == QStringLiteral("strict-code-update")) {
                    strictCodeFailure = error;
                    loop.quit();
                }
            });
        }) || !require(strictCodeFailure
                           == QStringLiteral("companion-key-operation-failed"),
                       "string response code was accepted as success")) return 1;

    const QString createSecret = QStringLiteral("sk-created-management-secret");
    CompanionKeyManagementApiTestAccess::install(client, manager, rawKeys());
    configSha = CompanionKeyManagementApiTestAccess::configurationSha(client);
    manager->enqueue(FakeResponse{200, response(groups())});
    QJsonObject createProjection;
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-create"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id, const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-create")) {
                    createProjection = projection;
                    loop.quit();
                }
            });
        })) return 1;
    const QJsonObject group = createProjection.value(QStringLiteral("groups"))
        .toArray().at(0).toObject();
    manager->enqueue(FakeResponse{200, response(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("created-key")},
        {QStringLiteral("key"), createSecret},
        {QStringLiteral("name"), QStringLiteral("Created")},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("group"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Codex")},
            {QStringLiteral("platform"), QStringLiteral("openai")}
        }}
    })});
    bool createStored = false;
    if (!waitFor([&]() {
            client.createCompanionApiKey(
                QStringLiteral("valid-create"), account, configSha,
                createProjection.value(QStringLiteral("projection_sha256")).toString(),
                group.value(QStringLiteral("create_handle")).toString(),
                group.value(QStringLiteral("group_handle")).toString(),
                QStringLiteral("Created"), 0);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyOperationCompleted,
                             &loop, [&](const QString &id, const QString &action, bool stored) {
                if (id == QStringLiteral("valid-create")
                        && action == QStringLiteral("create")) {
                    createStored = stored;
                    loop.quit();
                }
            });
        }) || !require(createStored, "created credential was not staged")) return 1;

    CompanionKeyManagementApiTestAccess::install(client, manager, rawKeys());
    configSha = CompanionKeyManagementApiTestAccess::configurationSha(client);
    manager->enqueue(FakeResponse{200, response(groups())});
    QJsonObject heldProjection;
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-held"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id, const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-held")) {
                    heldProjection = projection;
                    loop.quit();
                }
            });
        })) return 1;
    const QJsonObject heldKey = heldProjection.value(QStringLiteral("keys"))
        .toArray().at(0).toObject();
    manager->enqueue(FakeResponse{200, response(QJsonObject()),
                                  QByteArrayLiteral("application/json"), true});
    QString unknownOutcome;
    int heldCompleted = 0;
    QObject::connect(&client, &ApiClient::companionKeyOperationFailed, &client,
                     [&](const QString &id, const QString &, const QString &error) {
        if (id == QStringLiteral("held-delete")) unknownOutcome = error;
    });
    QObject::connect(&client, &ApiClient::companionKeyOperationCompleted, &client,
                     [&](const QString &id, const QString &, bool) {
        if (id == QStringLiteral("held-delete")) ++heldCompleted;
    });
    client.deleteCompanionApiKey(
        QStringLiteral("held-delete"), account,
        heldKey.value(QStringLiteral("key_identity")).toString(),
        heldKey.value(QStringLiteral("delete_handle")).toString(), configSha,
        heldProjection.value(QStringLiteral("projection_sha256")).toString());
    const int requestsWithHeldDelete = manager->requests.size();
    QString concurrentFailure;
    QObject::connect(&client, &ApiClient::companionKeyOperationFailed, &client,
                     [&](const QString &id, const QString &, const QString &error) {
        if (id == QStringLiteral("concurrent-update")) concurrentFailure = error;
    });
    client.updateCompanionApiKey(
        QStringLiteral("concurrent-update"), account,
        heldKey.value(QStringLiteral("key_identity")).toString(),
        heldKey.value(QStringLiteral("update_handle")).toString(), configSha,
        heldProjection.value(QStringLiteral("projection_sha256")).toString(),
        QJsonObject{{QStringLiteral("status"), QStringLiteral("inactive")}});
    if (!require(concurrentFailure == QStringLiteral("companion-key-management-busy")
                    && manager->requests.size() == requestsWithHeldDelete,
                 "concurrent mutation reached the network")) return 1;
    client.setAuthToken(QStringLiteral("other-login-token"));
    if (manager->heldReply) manager->heldReply->release();
    QCoreApplication::processEvents();
    if (!require(unknownOutcome == QStringLiteral("companion-key-outcome-unknown")
                    && heldCompleted == 0,
                 "uncertain delete outcome was reported as completed")) return 1;

    return 0;
}
