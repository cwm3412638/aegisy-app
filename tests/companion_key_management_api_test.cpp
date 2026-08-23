#include "api_client.h"
#include "companion_config_projection.h"
#include "companion_credential_broker.h"
#include "companion_key_management_projection.h"
#include "companion_model_projection.h"
#include "secure_storage.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QPointer>
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
    QByteArray authorization;
};

class FakeNetworkManager final : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

    void enqueue(const FakeResponse &response) { responses.enqueue(response); }

    QQueue<FakeResponse> responses;
    QList<CapturedRequest> requests;
    FakeReply *heldReply = nullptr;
    QList<QPointer<FakeReply>> heldReplies;

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        CapturedRequest captured{operation, request.url(), {}};
        if (outgoingData) captured.body = outgoingData->readAll();
        captured.authorization = request.rawHeader("Authorization");
        requests.append(captured);
        const FakeResponse response = responses.isEmpty()
            ? FakeResponse{500, QByteArrayLiteral("{\"code\":500}"),
                           QByteArrayLiteral("application/json"), false}
            : responses.dequeue();
        auto *reply = new FakeReply(request, operation, response, this);
        if (response.hold) {
            heldReply = reply;
            heldReplies.append(reply);
        }
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
            QStringLiteral("management-api-account-%1").arg(
                QCoreApplication::applicationPid()));
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
        client.m_currentCompanionModelProjections.clear();
        client.m_companionModelProjectionConfigurationSha256.clear();
        return account;
    }

    static QString configurationSha(const ApiClient &client)
    {
        return client.m_currentCompanionProjection.value(
            QStringLiteral("projection_sha256")).toString();
    }

    static QJsonObject configurationKey(const ApiClient &client, int index = 0)
    {
        const QJsonArray keys = client.m_currentCompanionProjection.value(
            QStringLiteral("keys")).toArray();
        return index >= 0 && index < keys.size()
            ? keys.at(index).toObject() : QJsonObject();
    }

    static int websiteModelAuthorityCount(const ApiClient &client)
    {
        return client.m_currentCompanionModelProjections.size();
    }

    static bool configurationAuthorityRetired(const ApiClient &client)
    {
        return client.m_currentCompanionProjection.isEmpty()
            && client.m_companionUsageSources.isEmpty()
            && client.m_currentCompanionKeyManagementProjection.isEmpty()
            && client.m_currentCompanionGroupSources.isEmpty()
            && client.m_currentCompanionModelProjections.isEmpty()
            && client.m_companionModelProjectionConfigurationSha256.isEmpty()
            && client.m_pendingCompanionModelRequests.isEmpty()
            && client.m_pendingCompanionUsageRequests.isEmpty()
            && client.m_pendingCompanionKeyOperations.isEmpty()
            && client.m_pendingCompanionKeyTests.isEmpty()
            && client.m_companionChatBinding.isEmpty()
            && client.m_companionImageBinding.isEmpty()
            && client.m_companionPresentationBindings.isEmpty();
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

    const QString cleanupSecret = QStringLiteral("sk-delete-cleanup-secret-sentinel");
    CompanionKeyManagementApiTestAccess::install(
        client, manager, rawKeys(cleanupSecret));
    configSha = CompanionKeyManagementApiTestAccess::configurationSha(client);
    const QJsonObject cleanupConfigKey =
        CompanionKeyManagementApiTestAccess::configurationKey(client);
    const QString cleanupKeyIdentity = cleanupConfigKey.value(
        QStringLiteral("key_identity")).toString();
    const QString cleanupCredentialHandle = cleanupConfigKey.value(
        QStringLiteral("credential_handle")).toString();
    manager->enqueue(FakeResponse{200, response(groups())});
    QJsonObject cleanupProjection;
    const bool cleanupReadFinished = waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-cleanup"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id, const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-cleanup")) {
                    cleanupProjection = projection;
                    loop.quit();
                }
            });
        });
    if (!require(cleanupReadFinished, "cleanup management read timed out")
            || !require(CompanionKeyManagementProjection::validate(cleanupProjection),
                       "cleanup management read failed")) return 1;
    const QJsonObject cleanupKey = cleanupProjection.value(QStringLiteral("keys"))
        .toArray().at(0).toObject();
    if (!require(CompanionCredentialBroker::resolve(
                     account, cleanupKeyIdentity, cleanupCredentialHandle)
                     == cleanupSecret,
                 "cleanup credential was not staged")) return 1;

    const int requestsBeforeCleanupDelete = manager->requests.size();
    manager->enqueue(FakeResponse{200, response(QJsonObject())});
    bool cleanupDeleteCompleted = false;
    bool cleanupReportedComplete = true;
    QString cleanupDeleteFailure;
    SecureStorage::failNextRemoveForTesting();
    const bool cleanupDeleteFinished = waitFor([&]() {
            client.deleteCompanionApiKey(
                QStringLiteral("cleanup-delete"), account, cleanupKeyIdentity,
                cleanupKey.value(QStringLiteral("delete_handle")).toString(),
                configSha, cleanupProjection.value(
                    QStringLiteral("projection_sha256")).toString());
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyOperationCompleted,
                             &loop, [&](const QString &id, const QString &action,
                                        bool cleanupComplete) {
                if (id == QStringLiteral("cleanup-delete")
                        && action == QStringLiteral("delete")) {
                    cleanupDeleteCompleted = true;
                    cleanupReportedComplete = cleanupComplete;
                    loop.quit();
                }
            });
            QObject::connect(&client, &ApiClient::companionKeyOperationFailed,
                             &loop, [&](const QString &id, const QString &,
                                        const QString &error) {
                if (id == QStringLiteral("cleanup-delete")) {
                    cleanupDeleteFailure = error;
                    loop.quit();
                }
            });
        });
    if (!require(cleanupDeleteFinished, "cleanup delete timed out")
            || !require(cleanupDeleteCompleted && !cleanupReportedComplete
                           && cleanupDeleteFailure.isEmpty(),
                       "local cleanup failure was not reported separately")) return 1;
    const CapturedRequest cleanupDelete = manager->requests.last();
    const QByteArray cleanupSafeEvidence =
        QJsonDocument(cleanupProjection).toJson(QJsonDocument::Compact)
        + cleanupDelete.url.toEncoded() + cleanupDelete.body;
    if (!require(manager->requests.size() == requestsBeforeCleanupDelete + 1
                    && cleanupDelete.operation
                        == QNetworkAccessManager::CustomOperation,
                 "cleanup failure changed remote delete dispatch")
            || !require(!cleanupSafeEvidence.contains(cleanupSecret.toUtf8()),
                        "cleanup credential entered safe projection or transport metadata")
            || !require(CompanionCredentialBroker::resolve(
                            account, cleanupKeyIdentity, cleanupCredentialHandle)
                            == cleanupSecret,
                        "failed cleanup removed the still-recoverable credential")
            || !require(CompanionCredentialBroker::forget(
                            account, cleanupKeyIdentity, cleanupCredentialHandle),
                        "normal cleanup after injected failure failed")
            || !require(CompanionCredentialBroker::resolve(
                            account, cleanupKeyIdentity, cleanupCredentialHandle).isEmpty(),
                        "credential remained after normal cleanup")) return 1;

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

    const QString originalTestCredential =
        QStringLiteral("sk-held-model-test-original-secret");
    CompanionKeyManagementApiTestAccess::install(
        client, manager, rawKeys(originalTestCredential));
    configSha = CompanionKeyManagementApiTestAccess::configurationSha(client);
    manager->enqueue(FakeResponse{200, response(groups())});
    QJsonObject heldTestProjection;
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-held-test"), account, configSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id,
                                        const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-held-test")) {
                    heldTestProjection = projection;
                    loop.quit();
                }
            });
        }) || !require(
            CompanionKeyManagementProjection::validate(heldTestProjection),
            "held Key-test management read failed")) return 1;
    const QJsonObject heldTestKey = heldTestProjection.value(
        QStringLiteral("keys")).toArray().at(0).toObject();

    const QString heldTestRequestId = QStringLiteral("held-model-key-test");
    int heldTestFailures = 0;
    int heldTestSuccesses = 0;
    int websiteModelObservations = 0;
    QString observedAccount;
    QString observedConfigurationSha;
    QString observedKeyIdentity;
    QString observedPlatform;
    QJsonObject observedModelProjection;
    qint64 observedModelAtMs = 0;
    QObject::connect(
        &client, &ApiClient::companionWebsiteModelsObserved, &client,
        [&](const QString &observedAccountIdentity,
            const QString &observedConfiguration,
            const QString &observedKey,
            const QString &observedModelPlatform,
            const QJsonObject &observedProjection,
            qint64 observedAtMs) {
            ++websiteModelObservations;
            observedAccount = observedAccountIdentity;
            observedConfigurationSha = observedConfiguration;
            observedKeyIdentity = observedKey;
            observedPlatform = observedModelPlatform;
            observedModelProjection = observedProjection;
            observedModelAtMs = observedAtMs;
        });
    QString heldTestFailureCode;
    QObject::connect(&client, &ApiClient::companionModelsFailed, &client,
                     [&](const QString &id, const QString &, const QString &error) {
        if (id == heldTestRequestId) {
            ++heldTestFailures;
            heldTestFailureCode = error;
        }
    });
    QObject::connect(&client, &ApiClient::companionModelsReceived, &client,
                     [&](const QString &id, const QString &, const QJsonObject &) {
        if (id == heldTestRequestId) ++heldTestSuccesses;
    });
    manager->enqueue(FakeResponse{
        200,
        QJsonDocument(QJsonObject{{
            QStringLiteral("data"),
            QJsonArray{QJsonObject{{QStringLiteral("id"),
                                    QStringLiteral("stale-model")}}}
        }}).toJson(QJsonDocument::Compact),
        QByteArrayLiteral("application/json"), true});
    const int requestsBeforeHeldTest = manager->requests.size();
    client.testCompanionApiKey(
        heldTestRequestId, account,
        heldTestKey.value(QStringLiteral("key_identity")).toString(),
        heldTestKey.value(QStringLiteral("test_handle")).toString(), configSha,
        heldTestProjection.value(QStringLiteral("projection_sha256")).toString());
    if (!require(manager->requests.size() == requestsBeforeHeldTest + 1,
                 "held Key-test did not reach the transport")) return 1;
    FakeReply *heldModelReply = manager->heldReply;
    const CapturedRequest heldModelRequest = manager->requests.last();
    if (!require(heldModelReply != nullptr,
                 "held Key-test reply was not captured")
            || !require(heldModelRequest.operation
                            == QNetworkAccessManager::GetOperation
                            && heldModelRequest.url.path()
                                == QStringLiteral("/v1/models"),
                        "held Key-test did not use the real models transport")
            || !require(heldModelRequest.authorization
                            == QByteArrayLiteral("Bearer ")
                                + originalTestCredential.toUtf8(),
                        "held Key-test did not resolve its original credential")) {
        return 1;
    }

    const QString rotatedTestCredential =
        QStringLiteral("sk-held-model-test-rotated-secret");
    QJsonArray rotatedRawKeys = rawKeys(rotatedTestCredential);
    QJsonObject rotatedRawKey = rotatedRawKeys.at(0).toObject();
    rotatedRawKey.insert(QStringLiteral("name"),
                         QStringLiteral("Managed Codex Rotated"));
    rotatedRawKeys.replace(0, rotatedRawKey);
    manager->enqueue(FakeResponse{200, response(QJsonObject{
        {QStringLiteral("items"), rotatedRawKeys},
        {QStringLiteral("total"), 1},
    })});
    QJsonObject rotatedConfiguration;
    if (!waitFor([&]() { client.getApiKeys(); }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionConfigurationReceived,
                             &loop, [&](const QJsonObject &projection) {
                rotatedConfiguration = projection;
                loop.quit();
            });
        }) || !require(CompanionConfigProjection::validate(rotatedConfiguration),
                       "rotated configuration refresh failed")) return 1;
    const QString rotatedConfigSha = rotatedConfiguration.value(
        QStringLiteral("projection_sha256")).toString();
    if (!require(rotatedConfigSha != configSha,
                 "configuration refresh did not rotate the projection")
            || !require(heldTestFailures == 1
                            && heldTestFailureCode
                                == QStringLiteral("companion-model-projection-changed")
                            && heldTestSuccesses == 0,
                        "configuration rotation did not retire the held Key-test")) {
        return 1;
    }

    manager->enqueue(FakeResponse{200, response(groups())});
    QJsonObject rotatedManagement;
    if (!waitFor([&]() {
            client.getCompanionKeyManagement(
                QStringLiteral("management-read-after-held-test"), account,
                rotatedConfigSha);
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionKeyManagementReceived,
                             &loop, [&](const QString &id,
                                        const QJsonObject &projection) {
                if (id == QStringLiteral("management-read-after-held-test")) {
                    rotatedManagement = projection;
                    loop.quit();
                }
            });
        }) || !require(CompanionKeyManagementProjection::validate(rotatedManagement),
                       "management refresh after held Key-test failed")) return 1;
    const QJsonObject rotatedTestKey = rotatedManagement.value(
        QStringLiteral("keys")).toArray().at(0).toObject();
    if (!require(rotatedManagement.value(QStringLiteral("projection_sha256"))
                        != heldTestProjection.value(QStringLiteral("projection_sha256"))
                    && rotatedTestKey.value(QStringLiteral("test_handle"))
                        != heldTestKey.value(QStringLiteral("test_handle")),
                 "management refresh did not rotate the Key-test capability")) return 1;

    heldModelReply->release();
    QCoreApplication::processEvents();
    if (!require(heldTestFailures == 1 && heldTestSuccesses == 0,
                 "released stale models response produced a second result")) return 1;

    const QString rotatedTestRequestId = QStringLiteral("rotated-model-key-test");
    QJsonObject rotatedModelProjection;
    QString rotatedModelFailure;
    manager->enqueue(FakeResponse{
        200,
        QJsonDocument(QJsonObject{{
            QStringLiteral("data"),
            QJsonArray{QJsonObject{{QStringLiteral("id"),
                                    QStringLiteral("fresh-model")}}}
        }}).toJson(QJsonDocument::Compact)});
    if (!waitFor([&]() {
            client.testCompanionApiKey(
                rotatedTestRequestId, account,
                rotatedTestKey.value(QStringLiteral("key_identity")).toString(),
                rotatedTestKey.value(QStringLiteral("test_handle")).toString(),
                rotatedConfigSha,
                rotatedManagement.value(QStringLiteral("projection_sha256")).toString());
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionModelsReceived, &loop,
                             [&](const QString &id, const QString &,
                                 const QJsonObject &projection) {
                if (id == rotatedTestRequestId) {
                    rotatedModelProjection = projection;
                    loop.quit();
                }
            });
            QObject::connect(&client, &ApiClient::companionModelsFailed, &loop,
                             [&](const QString &id, const QString &,
                                 const QString &error) {
                if (id == rotatedTestRequestId) {
                    rotatedModelFailure = error;
                    loop.quit();
                }
            });
        }) || !require(rotatedModelFailure.isEmpty()
                           && CompanionModelProjection::validate(
                               rotatedModelProjection)
                           && rotatedModelProjection.value(QStringLiteral("models"))
                               .toArray()
                               == QJsonArray{QStringLiteral("fresh-model")},
                       "rotated Key-test state was polluted by the old response")) {
        return 1;
    }
    const CapturedRequest rotatedModelRequest = manager->requests.last();
    if (!require(rotatedModelRequest.operation
                        == QNetworkAccessManager::GetOperation
                    && rotatedModelRequest.url.path()
                        == QStringLiteral("/v1/models")
                    && rotatedModelRequest.authorization
                        == QByteArrayLiteral("Bearer ")
                            + rotatedTestCredential.toUtf8()
                    && rotatedModelRequest.authorization
                        != heldModelRequest.authorization,
                 "rotated Key-test did not use the refreshed credential transport")) {
        return 1;
    }

    if (!require(CompanionKeyManagementApiTestAccess::websiteModelAuthorityCount(client)
                    == 0 && websiteModelObservations == 0,
                 "management Key-test entered website model authority")) return 1;

    const QJsonObject authorityKey = CompanionKeyManagementApiTestAccess::configurationKey(
        client);
    QJsonObject websiteModelProjection;
    manager->enqueue(FakeResponse{
        200,
        QJsonDocument(QJsonObject{{
            QStringLiteral("data"),
            QJsonArray{QJsonObject{{QStringLiteral("id"),
                                    QStringLiteral("website-authority-model")}}}
        }}).toJson(QJsonDocument::Compact)});
    if (!waitFor([&]() {
            client.getCompanionModels(
                QStringLiteral("website-model-authority"), account,
                authorityKey.value(QStringLiteral("key_identity")).toString(),
                authorityKey.value(QStringLiteral("credential_handle")).toString(),
                rotatedConfigSha,
                authorityKey.value(QStringLiteral("platform")).toString());
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionModelsReceived, &loop,
                             [&](const QString &id, const QString &,
                                 const QJsonObject &projection) {
                if (id == QStringLiteral("website-model-authority")) {
                    websiteModelProjection = projection;
                    loop.quit();
                }
            });
        }) || !require(CompanionModelProjection::validate(websiteModelProjection)
                           && CompanionKeyManagementApiTestAccess::websiteModelAuthorityCount(
                               client) == 1,
                       "ordinary website model result was not retained as current authority")) {
        return 1;
    }
    if (!require(websiteModelObservations == 1
                    && observedAccount == account
                    && observedConfigurationSha == rotatedConfigSha
                    && observedKeyIdentity
                        == authorityKey.value(QStringLiteral("key_identity")).toString()
                    && observedPlatform
                        == authorityKey.value(QStringLiteral("platform")).toString()
                    && observedModelProjection == websiteModelProjection
                    && observedModelAtMs > 0,
                 "ordinary website model observation binding was incomplete")) {
        return 1;
    }

    manager->enqueue(FakeResponse{
        200,
        QJsonDocument(QJsonObject{{
            QStringLiteral("data"),
            QJsonArray{QJsonObject{{QStringLiteral("id"),
                                    QStringLiteral("local-profile-model")}}}
        }}).toJson(QJsonDocument::Compact)});
    const QString localProfileIdentity = QStringLiteral("local-profile:sha256:")
        + QString(64, QLatin1Char('e'));
    if (!waitFor([&]() {
            client.getProfileModels(QStringLiteral("local-profile-model-request"),
                                    localProfileIdentity,
                                    QStringLiteral("sk-local-profile-model-secret"));
        }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionModelsReceived, &loop,
                             [&](const QString &id, const QString &,
                                 const QJsonObject &) {
                if (id == QStringLiteral("local-profile-model-request")) loop.quit();
            });
        }) || !require(CompanionKeyManagementApiTestAccess::websiteModelAuthorityCount(client)
                           == 1 && websiteModelObservations == 1,
                       "local Profile model result entered website model authority")) {
        return 1;
    }

    const int heldRetirementStart = manager->heldReplies.size();
    manager->enqueue(FakeResponse{200, response(QJsonObject()),
                                  QByteArrayLiteral("application/json"), true});
    manager->enqueue(FakeResponse{200, response(QJsonObject()),
                                  QByteArrayLiteral("application/json"), true});
    manager->enqueue(FakeResponse{
        200,
        QJsonDocument(QJsonObject{{
            QStringLiteral("data"),
            QJsonArray{QJsonObject{{QStringLiteral("id"),
                                    QStringLiteral("late-model")}}}
        }}).toJson(QJsonDocument::Compact),
        QByteArrayLiteral("application/json"), true});
    manager->enqueue(FakeResponse{200, QByteArrayLiteral("{}"),
                                  QByteArrayLiteral("application/json"), true});
    manager->enqueue(FakeResponse{200, QByteArrayLiteral("{}"),
                                  QByteArrayLiteral("application/json"), true});
    manager->enqueue(FakeResponse{200, QByteArrayLiteral("{}"),
                                  QByteArrayLiteral("application/json"), true});
    manager->enqueue(FakeResponse{200, response(QJsonObject(), 500)});

    const QString retirementDeleteId = QStringLiteral("retirement-held-delete");
    const QString retirementUsageId = QStringLiteral("retirement-held-usage");
    const QString retirementModelId = QStringLiteral("retirement-held-model");
    const QString retirementChatId = QStringLiteral("retirement-held-chat");
    const QString retirementImageId = QStringLiteral("retirement-held-image");
    const QString retirementPresentationId = QStringLiteral("retirement-held-presentation");
    int lateSuccesses = 0;
    QObject::connect(&client, &ApiClient::companionKeyOperationCompleted, &client,
                     [&](const QString &id, const QString &, bool) {
        if (id == retirementDeleteId) ++lateSuccesses;
    });
    QObject::connect(&client, &ApiClient::companionApiKeyUsageReceived, &client,
                     [&](const QString &id, const QJsonObject &) {
        if (id == retirementUsageId) ++lateSuccesses;
    });
    QObject::connect(&client, &ApiClient::companionModelsReceived, &client,
                     [&](const QString &id, const QString &, const QJsonObject &) {
        if (id == retirementModelId) ++lateSuccesses;
    });
    QObject::connect(&client, &ApiClient::chatCompleted, &client,
                     [&](const QString &id, const QString &) {
        if (id == retirementChatId) ++lateSuccesses;
    });
    QObject::connect(&client, &ApiClient::companionImageGenerated, &client,
                     [&](const QString &id, const QByteArray &, const QString &,
                         const QString &) {
        if (id == retirementImageId) ++lateSuccesses;
    });
    QObject::connect(&client, &ApiClient::presentationPlanReceived, &client,
                     [&](const QString &id, const QJsonObject &) {
        if (id == retirementPresentationId) ++lateSuccesses;
    });

    client.deleteCompanionApiKey(
        retirementDeleteId, account,
        rotatedTestKey.value(QStringLiteral("key_identity")).toString(),
        rotatedTestKey.value(QStringLiteral("delete_handle")).toString(),
        rotatedConfigSha,
        rotatedManagement.value(QStringLiteral("projection_sha256")).toString());
    client.getCompanionApiKeyUsage(retirementUsageId, account, rotatedConfigSha);
    client.getCompanionModels(
        retirementModelId, account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString());
    client.sendCompanionChatMessage(
        retirementChatId, account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString(),
        QStringLiteral("website-authority-model"), QJsonArray());
    client.generateCompanionImage(
        retirementImageId, account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString(),
        QStringLiteral("gpt-image-2"), QStringLiteral("test"),
        QStringLiteral("1024x1024"), QStringLiteral("auto"),
        QStringLiteral("png"));
    client.requestCompanionPresentationPlan(
        retirementPresentationId, account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString(),
        QStringLiteral("website-authority-model"), QStringLiteral("test"));

    int configurationFailureCount = 0;
    QString configurationFailureCode;
    if (!waitFor([&]() { client.getApiKeys(); }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionConfigurationFailed, &loop,
                             [&](const QString &error) {
                ++configurationFailureCount;
                configurationFailureCode = error;
                loop.quit();
            });
        }) || !require(configurationFailureCount == 1
                           && configurationFailureCode
                               == QStringLiteral("projection-response-invalid")
                           && CompanionKeyManagementApiTestAccess::configurationAuthorityRetired(
                               client),
                       "current configuration failure did not atomically retire authority")) {
        return 1;
    }

    const int requestsAfterRetirement = manager->requests.size();
    const QJsonObject retiredGroup = rotatedManagement.value(
        QStringLiteral("groups")).toArray().at(0).toObject();
    client.getCompanionApiKeyUsage(
        QStringLiteral("retired-usage"), account, rotatedConfigSha);
    client.getCompanionKeyManagement(
        QStringLiteral("retired-management"), account, rotatedConfigSha);
    client.createCompanionApiKey(
        QStringLiteral("retired-create"), account, rotatedConfigSha,
        rotatedManagement.value(QStringLiteral("projection_sha256")).toString(),
        retiredGroup.value(QStringLiteral("create_handle")).toString(),
        retiredGroup.value(QStringLiteral("group_handle")).toString(),
        QStringLiteral("Retired"), 0);
    client.updateCompanionApiKey(
        QStringLiteral("retired-update"), account,
        rotatedTestKey.value(QStringLiteral("key_identity")).toString(),
        rotatedTestKey.value(QStringLiteral("update_handle")).toString(),
        rotatedConfigSha,
        rotatedManagement.value(QStringLiteral("projection_sha256")).toString(),
        QJsonObject{{QStringLiteral("status"), QStringLiteral("inactive")}});
    client.deleteCompanionApiKey(
        QStringLiteral("retired-delete"), account,
        rotatedTestKey.value(QStringLiteral("key_identity")).toString(),
        rotatedTestKey.value(QStringLiteral("delete_handle")).toString(),
        rotatedConfigSha,
        rotatedManagement.value(QStringLiteral("projection_sha256")).toString());
    client.testCompanionApiKey(
        QStringLiteral("retired-test"), account,
        rotatedTestKey.value(QStringLiteral("key_identity")).toString(),
        rotatedTestKey.value(QStringLiteral("test_handle")).toString(),
        rotatedConfigSha,
        rotatedManagement.value(QStringLiteral("projection_sha256")).toString());
    client.getCompanionModels(
        QStringLiteral("retired-model"), account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString());
    client.sendCompanionChatMessage(
        QStringLiteral("retired-chat"), account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString(),
        QStringLiteral("website-authority-model"), QJsonArray());
    client.generateCompanionImage(
        QStringLiteral("retired-image"), account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString(),
        QStringLiteral("gpt-image-2"), QStringLiteral("test"),
        QStringLiteral("1024x1024"), QStringLiteral("auto"),
        QStringLiteral("png"));
    client.requestCompanionPresentationPlan(
        QStringLiteral("retired-presentation"), account,
        authorityKey.value(QStringLiteral("key_identity")).toString(),
        authorityKey.value(QStringLiteral("credential_handle")).toString(),
        rotatedConfigSha, authorityKey.value(QStringLiteral("platform")).toString(),
        QStringLiteral("website-authority-model"), QStringLiteral("test"));
    if (!require(manager->requests.size() == requestsAfterRetirement,
                 "retired companion authority reached the network")) return 1;

    for (int index = heldRetirementStart; index < manager->heldReplies.size(); ++index) {
        if (manager->heldReplies.at(index)) manager->heldReplies.at(index)->release();
    }
    QCoreApplication::processEvents();
    if (!require(lateSuccesses == 0
                    && CompanionKeyManagementApiTestAccess::configurationAuthorityRetired(
                        client),
                 "late retired companion replies restored authority or success")) return 1;

    CompanionKeyManagementApiTestAccess::install(client, manager, rawKeys());
    manager->enqueue(FakeResponse{200, response(QJsonObject(), 500),
                                  QByteArrayLiteral("application/json"), true});
    client.getApiKeys();
    QPointer<FakeReply> staleConfigurationReply = manager->heldReply;
    QJsonArray newestKeys = rawKeys(QStringLiteral("sk-newest-configuration-secret"));
    QJsonObject newestKey = newestKeys.at(0).toObject();
    newestKey.insert(QStringLiteral("name"), QStringLiteral("Newest Configuration"));
    newestKeys.replace(0, newestKey);
    manager->enqueue(FakeResponse{200, response(QJsonObject{
        {QStringLiteral("items"), newestKeys}, {QStringLiteral("total"), 1}
    })});
    QJsonObject newestProjection;
    if (!waitFor([&]() { client.getApiKeys(); }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionConfigurationReceived, &loop,
                             [&](const QJsonObject &projection) {
                newestProjection = projection;
                loop.quit();
            });
        }) || !require(CompanionConfigProjection::validate(newestProjection),
                       "newer configuration generation did not commit")) return 1;
    const QString newestSha = newestProjection.value(
        QStringLiteral("projection_sha256")).toString();
    int staleFailureSignals = 0;
    QObject::connect(&client, &ApiClient::companionConfigurationFailed, &client,
                     [&](const QString &) { ++staleFailureSignals; });
    if (staleConfigurationReply) staleConfigurationReply->release();
    QCoreApplication::processEvents();
    if (!require(staleFailureSignals == 0
                    && CompanionKeyManagementApiTestAccess::configurationSha(client)
                        == newestSha,
                 "stale configuration failure retired the newer authority")) return 1;

    manager->enqueue(FakeResponse{200, response(QJsonObject{
        {QStringLiteral("items"), newestKeys}, {QStringLiteral("total"), 0}
    })});
    int malformedContractFailures = 0;
    QString malformedContractCode;
    if (!waitFor([&]() { client.getApiKeys(); }, [&](QEventLoop &loop) {
            QObject::connect(&client, &ApiClient::companionConfigurationFailed, &loop,
                             [&](const QString &error) {
                ++malformedContractFailures;
                malformedContractCode = error;
                loop.quit();
            });
        }) || !require(malformedContractFailures == 1
                           && malformedContractCode
                               == QStringLiteral("projection-response-invalid")
                           && CompanionKeyManagementApiTestAccess::configurationAuthorityRetired(
                               client),
                       "contradictory Key total retained current authority")) {
        return 1;
    }

    return 0;
}
